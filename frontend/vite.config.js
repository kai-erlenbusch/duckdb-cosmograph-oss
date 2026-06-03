import { defineConfig } from 'vite';

export default defineConfig({
  build: {
    assetsInlineLimit: 100000000, // Inline small assets, though we'll handle the JS ourselves
    rollupOptions: {
      output: {
        entryFileNames: `assets/[name].js`,
        chunkFileNames: `assets/[name].js`,
        assetFileNames: `assets/[name].[ext]`
      }
    }
  }
});
