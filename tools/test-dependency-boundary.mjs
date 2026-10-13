/*
 * Validate built frontend archives against the installed core API surface.
 * Each platform group is: label nm-command core-archive frontend-archive...
 */

import { readFileSync } from "node:fs";
import { spawnSync } from "node:child_process";

const fail = (message) => {
  process.stderr.write(`FAIL dependency boundary: ${message}\n`);
  process.exitCode = 1;
};

const runNm = (command, args) => {
  const result = spawnSync(command, args, { encoding: "utf8" });
  if (result.error !== undefined) {
    throw result.error;
  }
  if (result.status !== 0) {
    throw new Error(
      `${command} ${args.join(" ")} failed: ${result.stderr.trim()}`,
    );
  }
  return result.stdout;
};

const normalizeSymbol = (line) => {
  const fields = line.trim().split(/\s+/u);
  let symbol = fields.at(-1) ?? "";

  if (symbol.startsWith("__imp_")) {
    symbol = symbol.slice("__imp_".length);
  }
  if (symbol.startsWith("_boxrig_") || symbol.startsWith("_utf8proc_")) {
    symbol = symbol.slice(1);
  }
  return symbol;
};

const symbolsFromNm = (output) =>
  new Set(
    output
      .split(/\r?\n/u)
      .map(normalizeSymbol)
      .filter((symbol) => symbol.length !== 0),
  );

const splitGroups = (arguments_) => {
  const groups = [[]];

  for (const argument of arguments_) {
    if (argument === "::") {
      groups.push([]);
    } else {
      groups.at(-1).push(argument);
    }
  }
  return groups;
};

const arguments_ = process.argv.slice(2);
const publicHeader = arguments_.shift();
if (publicHeader === undefined) {
  throw new Error("an installed public header is required");
}
const publicSource = readFileSync(publicHeader, "utf8");
const declarations = publicSource
  .replace(/\/\*[\s\S]*?\*\//gu, "")
  .replace(/\/\/.*$/gmu, "");
const publicSymbols = new Set(
  [...declarations.matchAll(/\b(boxrig_[A-Za-z0-9_]+)\s*\(/gu)].map(
    (match) => match[1],
  ),
);

if (/\butf8proc_/u.test(declarations)) {
  fail(`${publicHeader} exposes an utf8proc identifier`);
}

for (const group of splitGroups(arguments_)) {
  if (group.length < 4) {
    throw new Error(
      "each platform requires a label, nm command, core archive, and frontend archive",
    );
  }
  const [label, nmCommand, coreArchive, ...frontendArchives] = group;
  const coreSymbols = symbolsFromNm(
    runNm(nmCommand, ["-g", "--defined-only", coreArchive]),
  );

  for (const frontendArchive of frontendArchives) {
    const undefinedSymbols = symbolsFromNm(
      runNm(nmCommand, ["-u", frontendArchive]),
    );
    const privateCoreSymbols = [...undefinedSymbols]
      .filter(
        (symbol) =>
          symbol.startsWith("boxrig_") &&
          coreSymbols.has(symbol) &&
          !publicSymbols.has(symbol),
      )
      .sort();
    const utf8procSymbols = [...undefinedSymbols]
      .filter((symbol) => symbol.startsWith("utf8proc_"))
      .sort();

    if (privateCoreSymbols.length !== 0) {
      fail(
        `${label} ${frontendArchive} references private core symbols: ${privateCoreSymbols.join(", ")}`,
      );
    }
    if (utf8procSymbols.length !== 0) {
      fail(
        `${label} ${frontendArchive} references utf8proc symbols: ${utf8procSymbols.join(", ")}`,
      );
    }
  }
  if (process.exitCode === undefined) {
    process.stdout.write(`PASS dependency boundary ${label}\n`);
  }
}
