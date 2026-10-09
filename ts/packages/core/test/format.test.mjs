import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fmtBytes, fmtMode, splitExt, formatSize, partitionRole } from '../dist/index.js';

test('fmtBytes tiers', () => {
    assert.equal(fmtBytes(512), '512 B');
    assert.equal(fmtBytes(2048), '2048 B (2.0 KiB)');
    assert.match(fmtBytes(5 * 1024 * 1024), /5\.0 MiB/);
    assert.match(fmtBytes(3 * 1024 ** 3), /3\.00 GiB/);
});

test('fmtMode common cases', () => {
    assert.equal(fmtMode(0o100644), '-rw-r--r-- (0644)');
    assert.equal(fmtMode(0o040755), 'drwxr-xr-x (0755)');
    assert.equal(fmtMode(0o120777), 'lrwxrwxrwx (0777)');
    // fmtMode always prefixes a literal "0" to the octal of (mode & 0o7777),
    // so 4-digit special modes render as 5 chars: (04755), (01777).
    assert.equal(fmtMode(0o104755), '-rwsr-xr-x (04755)'); // setuid
    assert.equal(fmtMode(0o041777), 'drwxrwxrwt (01777)'); // sticky /tmp
});

test('splitExt — Chonky-bug-safe rules', () => {
    assert.equal(splitExt('a.txt'), '.txt');
    assert.equal(splitExt('noext'), ''); // no dot -> ''
    assert.equal(splitExt('.bashrc'), ''); // dotfile -> ''
    assert.equal(splitExt('.pwd.lock'), '.lock'); // later dot splits
    assert.equal(splitExt('trailing.'), ''); // trailing dot -> ''
});

test('formatSize adaptive units', () => {
    assert.equal(formatSize(undefined), '');
    assert.equal(formatSize(0), '0 B');
    assert.equal(formatSize(1536), '1.5 KiB');
    assert.equal(formatSize(10 * 1024 * 1024), '10 MiB');
});

test('partitionRole — GPT GUIDs and MBR type bytes', () => {
    // The Ubuntu cloud image layout: root, XBOOTLDR, BIOS boot, ESP.
    assert.equal(partitionRole('4f68bce3-e8cd-4db1-96e7-fbcaf984b709'), 'Linux root (x86-64)');
    assert.equal(partitionRole('bc13c2ff-59e6-4262-a352-b275fd6f7172'), 'Linux extended boot');
    assert.equal(partitionRole('21686148-6449-6e6f-744e-656564454649'), 'BIOS boot');
    assert.equal(partitionRole('C12A7328-F81F-11D2-BA4B-00A0C93EC93B'), 'EFI System');
    assert.equal(partitionRole('0x83'), 'Linux');
    assert.equal(partitionRole('0x0c'), 'FAT32');
    assert.equal(partitionRole('0xef'), 'EFI System');
    assert.equal(partitionRole('?'), ''); // engine's "unknown" sentinel
    assert.equal(partitionRole(''), '');
    assert.equal(partitionRole('0x42'), '');
    assert.equal(partitionRole('00000000-1111-2222-3333-444444444444'), '');
});
