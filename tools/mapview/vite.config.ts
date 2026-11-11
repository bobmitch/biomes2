import { defineConfig } from 'vite';

export default defineConfig({
  server: { port: 5173, strictPort: true },
  // src/generated/ is written by tools/spike/build.bat via tools/wgslgen.
  // It is a build product and is not checked in.
  build: { target: 'es2022' },
});
