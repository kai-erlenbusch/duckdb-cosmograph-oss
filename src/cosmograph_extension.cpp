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
#include <memory>
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

static std::string EscapeJSON(const std::string &input) {
    std::string output;
    for (char c : input) {
        if (c == '"') output += "\\\"";
        else if (c == '\\') output += "\\\\";
        else if (c == '\b') output += "\\b";
        else if (c == '\f') output += "\\f";
        else if (c == '\n') output += "\\n";
        else if (c == '\r') output += "\\r";
        else if (c == '\t') output += "\\t";
        else output += c;
    }
    return output;
}

static std::string WrapQuery(const std::string& query) {
    std::string q = query;
    q.erase(0, q.find_first_not_of(" \t\n\r"));
    if (q.length() >= 6) {
        std::string prefix = q.substr(0, 6);
        for (char &c : prefix) c = std::toupper(c);
        if (prefix == "SELECT" || prefix == "WITH  ") {
            return "(" + query + ") AS __t";
        }
    }
    return "(SELECT * FROM " + query + ") AS __t";
}

static void HandleDataQueryBinary(const httplib::Request& req, const std::string& query, duckdb::shared_ptr<DatabaseInstance> db, httplib::Response &res) {
    if (query.empty()) {
        uint32_t zero = 0;
        std::string buffer;
        buffer.append(reinterpret_cast<const char*>(&zero), 4);
        res.set_content(buffer, "application/octet-stream");
        return;
    }
    
    Connection con(*db);
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

    uint32_t total_rows = 0;
    std::string count_sql = "SELECT count(*)::BIGINT FROM " + query_safe + " WHERE 1=1";
    if (has_x) count_sql += " AND x IS NOT NULL";
    if (has_y) count_sql += " AND y IS NOT NULL";
    auto count_res = con.Query(count_sql);
    if (!count_res->HasError()) {
        total_rows = count_res->GetValue(0,0).GetValue<int64_t>();
    } else {
        res.status = 500;
        res.set_content(count_res->GetErrorObject().Message(), "text/plain");
        return;
    }

    auto result = con.SendQuery(sql + ";");
    if (result->HasError()) {
        res.status = 500;
        res.set_content(result->GetErrorObject().Message(), "text/plain");
        return;
    }

    std::shared_ptr<QueryResult> result_shared = std::move(result);
    auto total_rows_shared = std::make_shared<uint32_t>(total_rows);
    auto color_map_shared = std::make_shared<std::unordered_map<std::string, float>>();
    auto current_color_shared = std::make_shared<float>(0.0f);
    auto current_row_shared = std::make_shared<uint32_t>(0);
    auto header_written_shared = std::make_shared<bool>(false);

    res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
    res.set_content_provider(
        "application/octet-stream",
        [result_shared, total_rows_shared, color_map_shared, current_color_shared, current_row_shared, header_written_shared](size_t offset, httplib::DataSink &sink) mutable {
            if (!result_shared) {
                sink.done();
                return true;
            }

            if (!*header_written_shared) {
                uint32_t tr = *total_rows_shared;
                sink.write(reinterpret_cast<const char*>(&tr), 4);
                *header_written_shared = true;
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

            std::string local_buf;
            local_buf.resize(count * 20);
            char* data_ptr = &local_buf[0];
            
            uint32_t local_count = 0;

            for (idx_t i = 0; i < count; i++) {
                if (*current_row_shared >= *total_rows_shared) break;
                
                float x_val = x_data[i];
                float y_val = y_data[i];
                float size_val = size_data[i];
                uint32_t id_val = id_data[i];
                
                std::string c_str = "";
                if (color_validity.RowIsValid(i)) {
                    c_str = color_data[i].GetString();
                }
                if (color_map_shared->find(c_str) == color_map_shared->end()) {
                    (*color_map_shared)[c_str] = (*current_color_shared)++;
                }
                float color_val = (*color_map_shared)[c_str];

                // Interleaved packing
                char* row_ptr = data_ptr + (local_count * 20);
                *reinterpret_cast<float*>(row_ptr) = x_val;
                *reinterpret_cast<float*>(row_ptr + 4) = y_val;
                *reinterpret_cast<float*>(row_ptr + 8) = size_val;
                *reinterpret_cast<float*>(row_ptr + 12) = color_val;
                *reinterpret_cast<uint32_t*>(row_ptr + 16) = id_val;

                local_count++;
                (*current_row_shared)++;
            }
            
            if (local_count > 0) {
                sink.write(data_ptr, local_count * 20);
            }
            
            if (*current_row_shared >= *total_rows_shared) {
                result_shared.reset();
                sink.done();
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
    
    Connection con(*db);
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
        [result_shared, first_chunk_shared](size_t offset, httplib::DataSink &sink) mutable {
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

inline void ServeGraphFun(DataChunk &args, ExpressionState &state, Vector &result) {
    auto nodes_query = args.data[0].GetValue(0).ToString();
    std::string edges_query = "";
    std::string config_json = "{}";

    if (args.ColumnCount() == 2) {
        auto arg1 = args.data[1].GetValue(0).ToString();
        if (arg1.length() > 0 && arg1[0] == '{') {
            config_json = arg1;
        } else {
            edges_query = arg1;
        }
    } else if (args.ColumnCount() >= 3) {
        edges_query = args.data[1].GetValue(0).ToString();
        config_json = args.data[2].GetValue(0).ToString();
    }

    auto db = state.GetContext().db;

    std::lock_guard<std::mutex> lock(g_server_mutex);
    if (g_state.server) {
        for (idx_t i = 0; i < args.size(); i++) {
            result.SetValue(i, Value("Server is already running. Please SELECT stop_graph() first."));
        }
        return;
    }

    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    std::string token = std::to_string(dis(gen)) + std::to_string(dis(gen));
    g_state.token = token;

    g_state.server = duckdb::make_uniq<httplib::Server>();

    g_state.server->set_pre_routing_handler([](const httplib::Request &req, httplib::Response &res) {
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

    g_state.server->Get("/node_details", [db, nodes_query](const httplib::Request &req, httplib::Response &res) {
        bool has_id_val = req.has_param("id_val");
        if (!req.has_param("id") && !has_id_val) {
            res.status = 400; res.set_content("[]", "application/json"); return;
        }
        
        uint64_t target_val;
        try {
            if (has_id_val) {
                target_val = std::stoull(req.get_param_value("id_val"));
            } else {
                target_val = std::stoull(req.get_param_value("id"));
            }
        } catch (...) {
            res.status = 400; res.set_content("Invalid id parameter", "text/plain"); return;
        }

        Connection con(*db);
            std::string sql;
            if (has_id_val) {
                sql = "SELECT * FROM " + WrapQuery(nodes_query) + " WHERE id = " + std::to_string(target_val) + " LIMIT 1;";
            } else {
                sql = "SELECT * FROM " + WrapQuery(nodes_query) + " LIMIT 1 OFFSET " + std::to_string(target_val) + ";";
            }
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
        res.set_content("Please run SELECT stop_graph(); in your DuckDB client to stop the server.", "text/plain");
    });

    int port = 8080;
    while (!g_state.server->bind_to_port("127.0.0.1", port)) {
        port++;
        if (port > 8090) {
            for (idx_t i = 0; i < args.size(); i++) {
                result.SetValue(i, Value("Failed to bind to any port between 8080 and 8090."));
            }
            g_state.server.reset();
            return;
        }
    }
    
    std::cout << "Starting Cosmograph Server on http://127.0.0.1:" << port << "/?token=" << token << std::endl;
    std::cout << "Run SELECT stop_graph(); in DuckDB to kill the server." << std::endl;
    
    g_state.thread = duckdb::make_uniq<std::thread>([]() {
        g_state.server->listen_after_bind();
    });

    for (idx_t i = 0; i < args.size(); i++) {
        result.SetValue(i, Value("Cosmograph running at http://127.0.0.1:" + std::to_string(port) + "/?token=" + token));
    }
}

static void StopGraphFun(DataChunk &args, ExpressionState &state, Vector &result) {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    if (g_state.server) {
        g_state.server->stop();
        if (g_state.thread && g_state.thread->joinable()) {
            g_state.thread->join();
        }
        g_state.server.reset();
        g_state.thread.reset();
        for (idx_t i = 0; i < args.size(); i++) {
            result.SetValue(i, Value("Server stopped gracefully."));
        }
    } else {
        for (idx_t i = 0; i < args.size(); i++) {
            result.SetValue(i, Value("Server is not running."));
        }
    }
}
#else
static void ServeGraphFun(DataChunk &args, ExpressionState &state, Vector &result) {
    throw InvalidInputException("The Cosmograph server cannot run inside DuckDB-Wasm.");
}

static void StopGraphFun(DataChunk &args, ExpressionState &state, Vector &result) {
    throw InvalidInputException("The Cosmograph server cannot run inside DuckDB-Wasm.");
}
#endif

static void LoadInternal(ExtensionLoader &loader) {
    ScalarFunctionSet serve_graph_set("serve_graph");
    serve_graph_set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, ServeGraphFun));
    serve_graph_set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR, ServeGraphFun));
    serve_graph_set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR, ServeGraphFun));
    loader.RegisterFunction(serve_graph_set);

    ScalarFunctionSet stop_graph_set("stop_graph");
    stop_graph_set.AddFunction(ScalarFunction({}, LogicalType::VARCHAR, StopGraphFun));
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
