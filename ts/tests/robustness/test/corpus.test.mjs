import { test } from 'node:test';
import assert from 'node:assert/strict';
import { crc32c, crc32cRaw } from '../corpus/checksum.mjs';
import { randInt, rng } from '../corpus/rng.mjs';

test('crc32c matches the standard check value', () => {
    assert.equal(crc32c(Buffer.from('123456789')), 0xe3069283);
    assert.equal(crc32cRaw(0xffffffff, Buffer.from('123456789')), ~0xe3069283 >>> 0);
});

test('rng is deterministic per seed', () => {
    const a = rng(1);
    const b = rng(1);
    const c = rng(2);
    const sa = [a(), a(), a()];
    assert.deepEqual([b(), b(), b()], sa);
    assert.notDeepEqual([c(), c(), c()], sa);
    const next = rng(7);
    for (let i = 0; i < 1000; i++) {
        const n = randInt(next, 10);
        assert.ok(Number.isInteger(n) && n >= 0 && n < 10);
    }
});
