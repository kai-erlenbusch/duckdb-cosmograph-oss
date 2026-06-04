#define DUCKDB_EXTENSION_MAIN

#include "cosmograph_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "duckdb/parser/keyword_helper.hpp"

#ifndef __EMSCRIPTEN__
#include "httplib.h"
#include "include/web_assets.hpp"

#include <iostream>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <map>
#include <string_view>
#include <memory>
#include <cstring>
#include <random>

#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {

struct ServerState {
    std::unique_ptr<httplib::Server> server;
    std::unique_ptr<std::thread> thread;
    std::string token;
    
    ~ServerState() {
        if (server) {
            server->stop();
        }
        if (thread && thread->joinable()) {
            thread->join();
        }
    }
};

static ServerState g_state;
static std::mutex g_server_mutex;

static std::unique_ptr<Connection> GetNewConnection(duckdb::shared_ptr<DatabaseInstance> db) {
    return duckdb::make_uniq<Connection>(*db);
}


static std::string EscapeJSON(const std::string &input) {
    std::string output;
    output.reserve(input.length() + input.length() / 4);
    for (char c : input) {
        if (c == '"') output += "\\\"";
        else if (c == '\\') output += "\\\\";
        else if (c == '\b') output += "\\b";
        else if (c == '\f') output += "\\f";
        else if (c == '\n') output += "\\n";
        else if (c == '\r') output += "\\r";
        else if (c == '\t') output += "\\t";
        else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[7];
            snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
            output += buf;
        }
        else output += c;
    }
    return output;
}

static std::string WrapQuery(const std::string& query) {
    std::string q = query;
    size_t first = q.find_first_not_of(" \t\n\r");
    if (first != std::string::npos) {
        q.erase(0, first);
    } else {
        q.clear();
    }
    
    if (q.length() >= 4) {
        std::string prefix = q.substr(0, 6);
        for (char &c : prefix) c = std::toupper(c);
        if (prefix == "SELECT" || prefix.substr(0, 5) == "WITH ") {
            return "(" + query + ") AS __t";
        }
    }
    return "(SELECT * FROM " + query + ") AS __t";
}

class FastColorMap {
    std::vector<std::unique_ptr<std::string>> arena;
    struct KeyHash {
        std::size_t operator()(const string_t& k) const {
            std::size_t hash = 5381;
            const char* str = k.GetData();
            for (uint32_t i = 0; i < k.GetSize(); ++i) {
                hash = ((hash << 5) + hash) + str[i];
            }
            return hash;
        }
    };
    struct KeyEqual {
        bool operator()(const string_t& lhs, const string_t& rhs) const {
            if (lhs.GetSize() != rhs.GetSize()) return false;
            return memcmp(lhs.GetData(), rhs.GetData(), lhs.GetSize()) == 0;
        }
    };
    std::unordered_map<string_t, float, KeyHash, KeyEqual> map;
    float next_val = 0.0f;
public:
    float get_or_insert(const string_t& c_t) {
        auto it = map.find(c_t);
        if (it != map.end()) {
            return it->second;
        }
        auto new_str = std::unique_ptr<std::string>(new std::string(c_t.GetData(), c_t.GetSize()));
        string_t safe_key(new_str->data(), new_str->size());
        arena.push_back(std::move(new_str));
        float val = next_val++;
        map[safe_key] = val;
        return val;
    }
};

static void HandleDataQueryBinary(const httplib::Request& req, const std::string& query, duckdb::shared_ptr<DatabaseInstance> db, httplib::Response &res) {
    if (query.empty()) {
        uint32_t zero = 0;
        std::string buffer;
        buffer.append(reinterpret_cast<const char*>(&zero), 4);
        res.set_content(buffer, "application/octet-stream");
        return;
    }
    
    auto con_ptr = std::make_shared<std::unique_ptr<Connection>>(GetNewConnection(db));
    auto& con = **con_ptr;
    std::string query_safe = WrapQuery(query);

    auto check_res = con.Query("SELECT * FROM " + query_safe + " LIMIT 0;");
    if (check_res->HasError()) {
        res.status = 500;
        res.set_content(check_res->GetErrorObject().Message(), "text/plain");
        return;
    }

    bool has_x = false, has_y = false, has_size = false, has_color = false, has_id = false;
    for (auto& name : check_res->names) {
        if (name == "x") has_x = true;
        if (name == "y") has_y = true;
        if (name == "size") has_size = true;
        if (name == "color") has_color = true;
        if (name == "id") has_id = true;
    }

    std::string sql = "SELECT ";
    sql += has_x ? "CAST(x AS REAL) AS x, " : "0.0::REAL AS x, ";
    sql += has_y ? "CAST(y AS REAL) AS y, " : "0.0::REAL AS y, ";
    sql += has_size ? "COALESCE(CAST(size AS REAL), 5.0) AS size, " : "5.0::REAL AS size, ";
    sql += has_color ? "COALESCE(CAST(color AS VARCHAR), '') AS color, " : "''::VARCHAR AS color, ";
    sql += has_id ? "CAST(id AS UINTEGER) AS id " : "0::UINTEGER AS id ";
    sql += "FROM " + query_safe + " WHERE 1=1";
    if (has_x) sql += " AND x IS NOT NULL";
    if (has_y) sql += " AND y IS NOT NULL";

    bool has_bounds = req.has_param("min_x") && req.has_param("max_x") && req.has_param("min_y") && req.has_param("max_y");
    if (has_bounds && has_x && has_y) {
        try {
            double min_x = std::stod(req.get_param_value("min_x"));
            double max_x = std::stod(req.get_param_value("max_x"));
            double min_y = std::stod(req.get_param_value("min_y"));
            double max_y = std::stod(req.get_param_value("max_y"));
            sql += " AND x >= " + std::to_string(min_x) + 
                   " AND x <= " + std::to_string(max_x) + 
                   " AND y >= " + std::to_string(min_y) + 
                   " AND y <= " + std::to_string(max_y);
        } catch (...) {
            res.status = 400;
            res.set_content("Invalid bounds parameters", "text/plain");
            return;
        }
    }

    auto result = con.SendQuery(sql + ";");
    if (result->HasError()) {
        res.status = 500;
        res.set_content(result->GetErrorObject().Message(), "text/plain");
        return;
    }

    std::shared_ptr<QueryResult> result_shared = std::move(result);
    auto color_map_shared = std::make_shared<FastColorMap>();
    auto local_buf_shared = std::make_shared<std::string>();

    res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
    res.set_content_provider(
        "application/octet-stream",
        [con_ptr, result_shared, color_map_shared, local_buf_shared](size_t offset, httplib::DataSink &sink) mutable {
            if (!result_shared) {
                sink.done();
                return true;
            }
            
            auto chunk = result_shared->Fetch();
            if (!chunk || chunk->size() == 0) {
                result_shared.reset();
                sink.done();
                return true;
            }
            
            chunk->Flatten();
            idx_t count = chunk->size();
            
            auto x_data = FlatVector::GetData<float>(chunk->data[0]);
            auto y_data = FlatVector::GetData<float>(chunk->data[1]);
            auto size_data = FlatVector::GetData<float>(chunk->data[2]);
            auto color_data = FlatVector::GetData<string_t>(chunk->data[3]);
            auto& color_validity = FlatVector::Validity(chunk->data[3]);
            auto id_data = FlatVector::GetData<uint32_t>(chunk->data[4]);

            local_buf_shared->resize(count * 20);
            char* data_ptr = &(*local_buf_shared)[0];
            
            uint32_t local_count = 0;

            for (idx_t i = 0; i < count; i++) {
                float x_val = x_data[i];
                float y_val = y_data[i];
                float size_val = size_data[i];
                uint32_t id_val = id_data[i];
                
                float color_val;
                bool is_valid = color_validity.RowIsValid(i);
                
                if (is_valid) {
                    color_val = color_map_shared->get_or_insert(color_data[i]);
                } else {
                    color_val = color_map_shared->get_or_insert(string_t(""));
                }

                // Interleaved packing
                char* row_ptr = data_ptr + (local_count * 20);
                std::memcpy(row_ptr, &x_val, sizeof(float));
                std::memcpy(row_ptr + 4, &y_val, sizeof(float));
                std::memcpy(row_ptr + 8, &size_val, sizeof(float));
                std::memcpy(row_ptr + 12, &color_val, sizeof(float));
                std::memcpy(row_ptr + 16, &id_val, sizeof(uint32_t));

                local_count++;
            }
            
            if (local_count > 0) {
                sink.write(data_ptr, local_count * 20);
            }
            
            return true;
        }
    );
}

static void HandleEdgeQueryBinary(const httplib::Request& req, const std::string& query, duckdb::shared_ptr<DatabaseInstance> db, httplib::Response &res) {
    if (query.empty()) {
        uint32_t zero = 0;
        std::string buffer;
        buffer.append(reinterpret_cast<const char*>(&zero), 4);
        res.set_content(buffer, "application/octet-stream");
        return;
    }
    
    auto con_ptr = std::make_shared<std::unique_ptr<Connection>>(GetNewConnection(db));
    auto& con = **con_ptr;
    std::string query_safe = WrapQuery(query);

    auto check_res = con.Query("SELECT * FROM " + query_safe + " LIMIT 0;");
    if (check_res->HasError()) {
        res.status = 500;
        res.set_content(check_res->GetErrorObject().Message(), "text/plain");
        return;
    }

    bool has_source = false, has_target = false;
    for (auto& name : check_res->names) {
        if (name == "source") has_source = true;
        if (name == "target") has_target = true;
    }

    if (!has_source || !has_target) {
        res.status = 400;
        res.set_content("Edges query must return 'source' and 'target' columns.", "text/plain");
        return;
    }

    std::string sql = "SELECT CAST(source AS FLOAT) AS source, CAST(target AS FLOAT) AS target FROM " + query_safe + " WHERE source IS NOT NULL AND target IS NOT NULL";

    auto result = con.SendQuery(sql + ";");
    if (result->HasError()) {
        res.status = 500;
        res.set_content(result->GetErrorObject().Message(), "text/plain");
        return;
    }

    std::shared_ptr<QueryResult> result_shared = std::move(result);
    auto local_buf_shared = std::make_shared<std::string>();

    res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
    res.set_content_provider(
        "application/octet-stream",
        [con_ptr, result_shared, local_buf_shared](size_t offset, httplib::DataSink &sink) mutable {
            if (!result_shared) {
                sink.done();
                return true;
            }
            
            auto chunk = result_shared->Fetch();
            if (!chunk || chunk->size() == 0) {
                result_shared.reset();
                sink.done();
                return true;
            }
            
            chunk->Flatten();
            idx_t count = chunk->size();
            
            auto source_data = FlatVector::GetData<float>(chunk->data[0]);
            auto target_data = FlatVector::GetData<float>(chunk->data[1]);

            local_buf_shared->resize(count * 8);
            char* data_ptr = &(*local_buf_shared)[0];
            
            uint32_t local_count = 0;

            for (idx_t i = 0; i < count; i++) {
                float source_val = source_data[i];
                float target_val = target_data[i];
                
                char* row_ptr = data_ptr + (local_count * 8);
                std::memcpy(row_ptr, &source_val, sizeof(float));
                std::memcpy(row_ptr + 4, &target_val, sizeof(float));

                local_count++;
            }
            
            if (local_count > 0) {
                sink.write(data_ptr, local_count * 8);
            }
            
            return true;
        }
    );
}

static void HandleDataQuery(const httplib::Request& req, const std::string& query, duckdb::shared_ptr<DatabaseInstance> db, httplib::Response &res, bool is_nodes) {
    if (query.empty()) {
        res.set_content("[]", "application/json");
        return;
    }
    
    auto con_ptr = std::make_shared<std::unique_ptr<Connection>>(GetNewConnection(db));
    auto& con = **con_ptr;
    std::string query_safe = WrapQuery(query);
    std::string sql = "SELECT * FROM " + query_safe;

    if (is_nodes) {
        auto check_res = con.Query("SELECT * FROM " + query_safe + " LIMIT 0;");
        if (!check_res->HasError()) {
            bool has_x = false, has_y = false;
            for (auto& name : check_res->names) {
                if (name == "x") has_x = true;
                if (name == "y") has_y = true;
            }
            bool has_bounds = req.has_param("min_x") && req.has_param("max_x") && req.has_param("min_y") && req.has_param("max_y");
            if (has_bounds && has_x && has_y) {
                try {
                    double min_x = std::stod(req.get_param_value("min_x"));
                    double max_x = std::stod(req.get_param_value("max_x"));
                    double min_y = std::stod(req.get_param_value("min_y"));
                    double max_y = std::stod(req.get_param_value("max_y"));
                    sql += " WHERE x >= " + std::to_string(min_x) + 
                           " AND x <= " + std::to_string(max_x) + 
                           " AND y >= " + std::to_string(min_y) + 
                           " AND y <= " + std::to_string(max_y);
                } catch (...) {
                    res.status = 400;
                    res.set_content("Invalid bounds parameters", "text/plain");
                    return;
                }
            }
        }
    }

    auto result = con.SendQuery(sql + ";");
    if (result->HasError()) {
        res.status = 500;
        res.set_content(result->GetErrorObject().Message(), "text/plain");
        return;
    }

    std::shared_ptr<QueryResult> result_shared = std::move(result);
    auto first_chunk_shared = std::make_shared<bool>(true);

    res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
    res.set_content_provider(
        "application/json",
        [con_ptr, result_shared, first_chunk_shared](size_t offset, httplib::DataSink &sink) mutable {
            if (!result_shared) {
                sink.done();
                return true;
            }

            if (*first_chunk_shared) {
                sink.write("[", 1);
            }
            
            auto chunk = result_shared->Fetch();
            if (!chunk || chunk->size() == 0) {
                if (*first_chunk_shared) {
                    sink.write("]", 1);
                } else {
                    sink.write("\n]", 2);
                }
                result_shared.reset();
                sink.done();
                return true;
            }

            chunk->Flatten();
            idx_t count = chunk->size();
            std::string local_buf;
            
            for (idx_t i = 0; i < count; i++) {
                if (*first_chunk_shared && i == 0) {
                    local_buf += "\n  {";
                } else {
                    local_buf += ",\n  {";
                }
                
                for (idx_t col_idx = 0; col_idx < result_shared->names.size(); col_idx++) {
                    if (col_idx > 0) local_buf += ",";
                    local_buf += "\"" + EscapeJSON(result_shared->names[col_idx]) + "\":";
                    Value val = chunk->GetValue(col_idx, i);
                    if (val.IsNull()) {
                        local_buf += "null";
                    } else if (val.type().IsNumeric()) {
                        local_buf += val.ToString();
                    } else if (val.type().id() == LogicalTypeId::BOOLEAN) {
                        local_buf += val.GetValue<bool>() ? "true" : "false";
                    } else {
                        local_buf += "\"" + EscapeJSON(val.ToString()) + "\"";
                    }
                }
                local_buf += "}";
            }
            
            sink.write(local_buf.data(), local_buf.length());
            *first_chunk_shared = false;
            return true;
        }
    );
}

struct ServeGraphBindData : public TableFunctionData {
    std::string nodes_query;
    std::string edges_query;
    std::string config_json;
};

static unique_ptr<FunctionData> ServeGraphBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
    auto result = make_uniq<ServeGraphBindData>();
    
    result->nodes_query = input.inputs[0].ToString();
    if (input.inputs.size() == 2) {
        auto arg1 = input.inputs[1].ToString();
        auto trimmed = arg1;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
        if (!trimmed.empty() && trimmed[0] == '{') {
            result->config_json = arg1;
        } else {
            result->edges_query = arg1;
        }
    } else if (input.inputs.size() >= 3) {
        result->edges_query = input.inputs[1].ToString();
        result->config_json = input.inputs[2].ToString();
    }

    return_types.push_back(LogicalType::VARCHAR);
    names.emplace_back("status");
    return std::move(result);
}

struct ServeGraphState : public GlobalTableFunctionState {
    bool finished = false;
};

static unique_ptr<GlobalTableFunctionState> ServeGraphInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<ServeGraphState>();
}

static void ServeGraphFun(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &state = data.global_state->Cast<ServeGraphState>();
    if (state.finished) {
        output.SetCardinality(0);
        return;
    }

    auto &bind_data = data.bind_data->Cast<ServeGraphBindData>();
    auto nodes_query = bind_data.nodes_query;
    auto edges_query = bind_data.edges_query;
    auto config_json = bind_data.config_json;

    auto db = context.db;

    std::lock_guard<std::mutex> lock(g_server_mutex);
    if (g_state.server) {
        output.SetCardinality(1);
        output.SetValue(0, 0, Value("Server is already running. Please SELECT * FROM stop_graph() first."));
        state.finished = true;
        return;
    }

    std::random_device rd;
    std::string token;
    for(int i=0; i<4; i++) {
        uint32_t val = rd();
        char buf[9];
        snprintf(buf, sizeof(buf), "%08x", val);
        token += buf;
    }
    g_state.token = token;

    g_state.server = duckdb::make_uniq<httplib::Server>();

    g_state.server->set_pre_routing_handler([](const httplib::Request &req, httplib::Response &res) {
        res.set_header("Content-Security-Policy", 
            "default-src 'self'; "
            "script-src 'self' 'unsafe-inline' 'unsafe-eval'; "
            "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
            "font-src 'self' https://fonts.gstatic.com; "
            "connect-src 'self'; "
            "worker-src 'self' blob:;"
        );
        res.set_header("X-Content-Type-Options", "nosniff");

        if (req.path.find("/data/") == 0 || req.path == "/config" || req.path == "/node_details") {
            std::string auth_header = req.get_header_value("Authorization");
            std::string req_token = req.get_param_value("token");
            if (req_token != g_state.token && auth_header != "Bearer " + g_state.token) {
                res.status = 401;
                res.set_content("Unauthorized", "text/plain");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    auto assets = duckdb::web_assets::get_assets();
    for (const auto& kv : assets) {
        std::string path = kv.first;
        if (path == "/index.html") {
             g_state.server->Get("/", [kv](const httplib::Request &, httplib::Response &res) {
                 res.set_content(reinterpret_cast<const char*>(kv.second.data), kv.second.size, kv.second.content_type);
                 res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
             });
        }
        g_state.server->Get(path, [kv](const httplib::Request &, httplib::Response &res) {
             res.set_content(reinterpret_cast<const char*>(kv.second.data), kv.second.size, kv.second.content_type);
             res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        });
    }

    g_state.server->Get("/config", [config_json](const httplib::Request &, httplib::Response &res) {
        std::string json = "{\"config\": " + (config_json.empty() ? "{}" : config_json) + "}";
        res.set_content(json, "application/json");
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
    });

    g_state.server->Get("/data/nodes", [db, nodes_query](const httplib::Request &req, httplib::Response &res) {
        HandleDataQuery(req, nodes_query, db, res, true);
    });

    g_state.server->Get("/data/nodes_binary", [db, nodes_query](const httplib::Request &req, httplib::Response &res) {
        HandleDataQueryBinary(req, nodes_query, db, res);
    });

    g_state.server->Get("/data/edges", [db, edges_query](const httplib::Request &req, httplib::Response &res) {
        HandleDataQuery(req, edges_query, db, res, false);
    });

    g_state.server->Get("/data/edges_binary", [db, edges_query](const httplib::Request &req, httplib::Response &res) {
        HandleEdgeQueryBinary(req, edges_query, db, res);
    });

    g_state.server->Get("/node_details", [db, nodes_query](const httplib::Request &req, httplib::Response &res) {
        bool has_id_val = req.has_param("id_val");
        if (!has_id_val) {
            res.status = 400; 
            res.set_content("Cosmograph Error: Click interactions on nodes require an explicit ID column in your query to prevent O(N) scans.", "text/plain"); 
            return;
        }
        
        uint64_t target_val;
        try {
            target_val = std::stoull(req.get_param_value("id_val"));
        } catch (...) {
            res.status = 400; res.set_content("Invalid id parameter", "text/plain"); return;
        }

        auto con_ptr = std::make_shared<std::unique_ptr<Connection>>(GetNewConnection(db));
        auto& con = **con_ptr;
        std::string sql = "SELECT * FROM " + WrapQuery(nodes_query) + " WHERE id = " + std::to_string(target_val) + " LIMIT 1;";
        
        auto result = con.Query(sql);
        if (result->HasError()) {
            res.status = 500;
            res.set_content(result->GetErrorObject().Message(), "text/plain");
            return;
        }
            std::string json = "[";
            for (idx_t row_idx = 0; row_idx < result->RowCount(); row_idx++) {
                if (row_idx > 0) json += ",";
                json += "{";
                for (idx_t col_idx = 0; col_idx < result->ColumnCount(); col_idx++) {
                    if (col_idx > 0) json += ",";
                    json += "\"" + EscapeJSON(result->names[col_idx]) + "\":";
                    Value val = result->GetValue(col_idx, row_idx);
                    if (val.IsNull()) {
                        json += "null";
                    } else if (val.type().IsNumeric()) {
                        json += val.ToString();
                    } else if (val.type().id() == LogicalTypeId::BOOLEAN) {
                        json += val.GetValue<bool>() ? "true" : "false";
                    } else {
                        json += "\"" + EscapeJSON(val.ToString()) + "\"";
                    }
                }
                json += "}";
            }
            json += "]";
            res.set_content(json, "application/json");
    });

    g_state.server->Get("/stop", [&](const httplib::Request &, httplib::Response &res) {
        res.set_content("Please run SELECT * FROM stop_graph(); in your DuckDB client to stop the server.", "text/plain");
    });

    int port = 8080;
    while (!g_state.server->bind_to_port("127.0.0.1", port)) {
        port++;
        if (port > 8090) {
            output.SetCardinality(1);
            output.SetValue(0, 0, Value("Failed to bind to any port between 8080 and 8090."));
            g_state.server.reset();
            state.finished = true;
            return;
        }
    }
    
    std::cout << "Starting Cosmograph Server on http://127.0.0.1:" << port << "/?token=" << token << std::endl;
    std::cout << "Run SELECT * FROM stop_graph(); in DuckDB to kill the server." << std::endl;
    
    g_state.thread = duckdb::make_uniq<std::thread>([]() {
        g_state.server->listen_after_bind();
    });

    output.SetCardinality(1);
    output.SetValue(0, 0, Value("Cosmograph running at http://127.0.0.1:" + std::to_string(port) + "/?token=" + token));
    state.finished = true;
}

struct StopGraphState : public GlobalTableFunctionState {
    bool finished = false;
};

static unique_ptr<GlobalTableFunctionState> StopGraphInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<StopGraphState>();
}

static unique_ptr<FunctionData> StopGraphBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
    return_types.push_back(LogicalType::VARCHAR);
    names.emplace_back("status");
    return make_uniq<TableFunctionData>();
}

static void StopGraphFun(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &state = data.global_state->Cast<StopGraphState>();
    if (state.finished) {
        output.SetCardinality(0);
        return;
    }

    std::lock_guard<std::mutex> lock(g_server_mutex);
    if (g_state.server) {
        g_state.server->stop();
        if (g_state.thread && g_state.thread->joinable()) {
            g_state.thread->join();
        }
        g_state.server.reset();
        g_state.thread.reset();
        
        g_state.thread.reset();
        output.SetValue(0, 0, Value("Server stopped gracefully."));
    } else {
        output.SetValue(0, 0, Value("Server is not running."));
    }
    output.SetCardinality(1);
    state.finished = true;
}
#else
static void ServeGraphFun(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    throw InvalidInputException("The Cosmograph server cannot run inside DuckDB-Wasm.");
}

static void StopGraphFun(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    throw InvalidInputException("The Cosmograph server cannot run inside DuckDB-Wasm.");
}
#endif

static void LoadInternal(ExtensionLoader &loader) {
    TableFunctionSet serve_graph_set("serve_graph");
    serve_graph_set.AddFunction(TableFunction("serve_graph", {LogicalType::VARCHAR}, ServeGraphFun, ServeGraphBind, ServeGraphInitGlobal));
    serve_graph_set.AddFunction(TableFunction("serve_graph", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ServeGraphFun, ServeGraphBind, ServeGraphInitGlobal));
    serve_graph_set.AddFunction(TableFunction("serve_graph", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, ServeGraphFun, ServeGraphBind, ServeGraphInitGlobal));
    loader.RegisterFunction(serve_graph_set);

    TableFunctionSet stop_graph_set("stop_graph");
    stop_graph_set.AddFunction(TableFunction("stop_graph", {}, StopGraphFun, StopGraphBind, StopGraphInitGlobal));
    loader.RegisterFunction(stop_graph_set);
}

void CosmographExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string CosmographExtension::Name() {
    return "cosmograph";
}

std::string CosmographExtension::Version() const {
#ifdef EXT_VERSION_COSMOGRAPH
    return EXT_VERSION_COSMOGRAPH;
#else
    return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(cosmograph, loader) {
    duckdb::LoadInternal(loader);
}
}
