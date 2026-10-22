'use strict'
// extractStrings must not read outside the ArrayBuffer it is given
const assert = require('node:assert/strict')
const { extractStrings } = require(process.argv[2] || '../index.js')

const oneByte = new ArrayBuffer(1)
new Uint8Array(oneByte)[0] = 0xbf // fixstr declaring 31 bytes
assert.throws(() => extractStrings(0, 32, oneByte), RangeError, 'end past the ArrayBuffer')
assert.throws(() => extractStrings(1, 0, oneByte), RangeError, 'start past end')
assert.throws(() => extractStrings(0, 1), TypeError, 'missing ArrayBuffer')
assert.throws(() => extractStrings('a', 1, oneByte), TypeError, 'non-numeric start')

const valid = Uint8Array.of(0xa1, 0x61)
assert.equal(extractStrings(0, valid.length, valid.buffer), 'a')
console.log('extractStrings range checks passed')
