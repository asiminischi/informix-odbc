'use strict';

// Copies the C++ sources the addon compiles (the sqli/ protocol layer plus
// two ODBC-free helpers from driver/) from the surrounding informix-odbc
// checkout into vendor/, so the npm tarball is self-contained. In an
// installed package there is no checkout around it and vendor/ already
// ships in the tarball, so this is a no-op there.

const fs = require('node:fs');
const path = require('node:path');

const pkgRoot = path.join(__dirname, '..');
const repoRoot = path.join(pkgRoot, '..');
const vendor = path.join(pkgRoot, 'vendor');

const FILES = [
  ...fs.existsSync(path.join(repoRoot, 'sqli'))
    ? fs.readdirSync(path.join(repoRoot, 'sqli')).filter((f) => /\.(cpp|h)$/.test(f)).map((f) => `sqli/${f}`)
    : [],
  'driver/convert.cpp',
  'driver/convert.h',
  'driver/insert_table_name.cpp',
  'driver/insert_table_name.h',
];

if (!fs.existsSync(path.join(repoRoot, 'sqli', 'connection.h'))) {
  if (fs.existsSync(path.join(vendor, 'sqli', 'connection.h'))) process.exit(0);
  console.error('informix-native: vendor/ is missing and no informix-odbc checkout was found next to this package');
  process.exit(1);
}

fs.rmSync(vendor, { recursive: true, force: true });
for (const rel of FILES) {
  const dest = path.join(vendor, rel);
  fs.mkdirSync(path.dirname(dest), { recursive: true });
  fs.copyFileSync(path.join(repoRoot, rel), dest);
}
