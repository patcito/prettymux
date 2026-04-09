# Development conventions

## Bun first

Prefer Bun over Node.js tooling in this repo:

- `bun <file>` instead of `node <file>`
- `bun test` instead of Jest or Vitest
- `bun build <file.html|file.ts|file.css>` instead of webpack or esbuild
- `bun install` instead of `npm install`, `yarn install`, or `pnpm install`
- `bun run <script>` instead of `npm run <script>` and equivalents
- `bunx <package> <command>` instead of `npx <package> <command>`

Bun loads `.env` automatically, so do not add `dotenv` just for that.

## API and backend preferences

- Prefer `Bun.serve()` instead of Express
- Prefer `bun:sqlite` instead of `better-sqlite3`
- Prefer `Bun.redis` instead of `ioredis`
- Prefer `Bun.sql` instead of `pg` or `postgres.js`
- Prefer built-in `WebSocket` instead of `ws`
- Prefer `Bun.file` over `node:fs` `readFile` or `writeFile`
- Prefer `Bun.$\`...\`` over `execa`

## Testing

Use Bun's test runner:

```ts
import { expect, test } from "bun:test";

test("hello world", () => {
  expect(1).toBe(1);
});
```

Run tests with:

```bash
bun test
```

## Frontend guidance

- Use HTML imports with `Bun.serve()`
- Do not add Vite unless there is a compelling need
- Bun can import HTML, TSX, JSX, JS, and CSS directly

Minimal example:

```ts
import index from "./index.html";

Bun.serve({
  routes: {
    "/": index,
  },
  development: {
    hmr: true,
    console: true,
  },
});
```

HTML can load TSX directly:

```html
<html>
  <body>
    <script type="module" src="./frontend.tsx"></script>
  </body>
</html>
```

Run with:

```bash
bun --hot ./index.ts
```
