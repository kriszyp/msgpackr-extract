'use strict'
const assert = require('node:assert/strict')
const { extractStrings } = require(process.argv[2] || '../index.js')

const text = (string) => {
	const bytes = Buffer.from(string)
	return Buffer.concat([Buffer.from([0xa0 | bytes.length]), bytes])
}
const extract = (source) => extractStrings(0, source.length, new Uint8Array(source).buffer)
assert.deepEqual(extract(Buffer.concat([text('abc'), text('é!')])), ['abc', 'é!'])
// more than 40 bytes between short strings flushes the first one
assert.deepEqual(extract(Buffer.concat([text('abc'), Buffer.alloc(41, 0x01), text('def')])), ['abc', 'def'])
assert.equal(extract(Buffer.concat([Buffer.from([0xda, 0x01, 0x00]), Buffer.alloc(256, 0x62)])), 'b'.repeat(256))
console.log('extractStrings string creation passed')
