#!/usr/bin/env node

import { readFile } from 'node:fs/promises';

const fail = (message) => {
  process.stderr.write(`FAIL Win32 reconversion host report: ${message}\n`);
  process.exitCode = 1;
};

if (process.argv.length !== 3) {
  fail('expected one report path');
} else {
  const report = JSON.parse(await readFile(process.argv[2], 'utf8'));

  if (
    report.schema !== 1 ||
    report.ready !== true ||
    report.documentIdentity !== 7001 ||
    report.documentRevision !== 2 ||
    report.contextOffset !== 100 ||
    report.context !== 'before 再変換 after' ||
    report.selectionOffset !== 107 ||
    report.selectionLength !== 9 ||
    report.selectedText !== '再変換' ||
    report.replacementCount !== 1 ||
    report.lastRequestId !== 1 ||
    report.lastBaseRevision !== 1 ||
    report.lastRangeOffset !== 107 ||
    report.lastRangeLength !== 9 ||
    report.lastReplacement !== '再変換' ||
    report.lastOutcome !== 'accepted' ||
    report.terminalOutput !== ''
  ) {
    fail(`unexpected payload ${JSON.stringify(report)}`);
  } else {
    process.stdout.write(
      'PASS dedicated Win32 reconversion host exposes document transactions\n'
    );
  }
}
