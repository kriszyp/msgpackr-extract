#!/usr/bin/env node

const { dirname } = require('path');
const { fileURLToPath } = require('url');
process.chdir(dirname(__dirname));
process.argv[2] = 'download';
require('prebuildify-ci');
