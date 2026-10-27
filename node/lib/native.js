'use strict';

// Loads the compiled addon: a prebuild from prebuilds/ if one matches this
// platform, otherwise build/Release (built from source on `npm install`).
const path = require('node:path');

module.exports = require('node-gyp-build')(path.join(__dirname, '..'));
