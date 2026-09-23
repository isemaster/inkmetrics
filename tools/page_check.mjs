// Прогон скрипта страницы прибора вне браузера: ловим синтаксис и смотрим, что
// именно попадёт в строки таблицы при разных состояниях агента.
// Использование: node page_check.mjs page.html state.json
import { readFileSync } from "node:fs";

const [pagePath, statePath] = process.argv.slice(2);
const html = readFileSync(pagePath, "utf8");
const state = JSON.parse(readFileSync(statePath, "utf8"));

const m = html.match(/<script>([\s\S]*?)<\/script>/);
if (!m) { console.log("ОШИБКА: на странице нет <script>"); process.exit(1); }
const js = m[1];

let captured = "";
globalThis.document = { getElementById: () => ({ set innerHTML(v) { captured = v; }, get innerHTML() { return captured; } }) };
globalThis.fetch = async (url) => {
  if (String(url).startsWith("/api/state")) return { json: async () => state };
  return { json: async () => ({}) };
};
globalThis.setInterval = () => 0;

try {
  await eval(`(async () => { ${js} ; await new Promise(r => setTimeout(r, 80)); })()`);
} catch (e) {
  console.log("ОШИБКА исполнения:", e.message);
  process.exit(1);
}

const rows = [...captured.matchAll(/<tr><td>(.*?)<\/td><td>(.*?)<\/td><\/tr>/g)];
console.log("строк в таблице:", rows.length);
for (const [, k, v] of rows) console.log(`  ${k.padEnd(32)} ${v}`);
if (captured === "" ) console.log("ВНИМАНИЕ: таблица не заполнена");
