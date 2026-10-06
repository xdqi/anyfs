/** mulberry32: a tiny seeded PRNG. Same seed, same stream, on every host. */
export function rng(seed) {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6d2b79f5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

/** An integer in [0, n) from `next`. */
export const randInt = (next, n) => Math.floor(next() * n);
