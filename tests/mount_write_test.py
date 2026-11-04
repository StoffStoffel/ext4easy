# Write test through a real drive letter: mounts an image writable with
# "ext4easy mount --write", changes files only through normal Windows file
# APIs, unmounts with Ctrl+Break (clean unmount), remounts read-only and
# compares everything with an in-memory model. Writes a manifest that
# tests/verify_image.sh checks on Linux afterwards.
#
#   python tests/mount_write_test.py <ext4easy.exe> <image> <X:> <manifest> [--seed N] [--ops N]
#                                    [--crash]
#
# --crash: instead of unmounting, waits a few seconds (the background flush
# runs every 2 s) and kills the mount process. Linux must then only replay
# the journal, and all data must be there.
import argparse
import ctypes
import hashlib
import os
import random
import shutil
import signal
import subprocess
import sys
import time


def wait_for_drive(drive, proc, present=True, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        if os.path.isdir(drive + '\\') == present:
            return True
        if present and proc.poll() is not None:
            return False
        time.sleep(0.1)
    return False


def mount(exe, image, drive, write):
    args = [exe, 'mount', image, drive] + (['--write'] if write else [])
    p = subprocess.Popen(args, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if not wait_for_drive(drive, p):
        out = p.communicate(timeout=5)[0] if p.poll() is not None else ''
        sys.exit(f'FAIL: mount did not appear: {out}')
    return p


def unmount(p, drive):
    p.send_signal(signal.CTRL_BREAK_EVENT)  # handled like Ctrl+C: clean unmount
    out = p.communicate(timeout=60)[0]
    if p.returncode != 0 or 'Unmounted' not in out:
        sys.exit(f'FAIL: unmount: exit {p.returncode}: {out}')
    wait_for_drive(drive, p, present=False)


class Model:
    def __init__(self, root):
        self.root = root  # "W:\\"
        self.files = {}   # rel path ("a/b.txt") -> bytes
        self.dirs = set()

    def win(self, rel):
        return os.path.join(self.root, rel.replace('/', '\\'))

    def check_all(self, label):
        errors = 0
        for rel, data in self.files.items():
            try:
                with open(self.win(rel), 'rb') as f:
                    got = f.read()
            except OSError as e:
                print(f'[{label}] cannot read {rel}: {e}')
                errors += 1
                continue
            if got != data:
                print(f'[{label}] content differs: {rel} ({len(got)} vs {len(data)} bytes)')
                errors += 1
        have_files, have_dirs = set(), set()
        for dirpath, dirnames, filenames in os.walk(self.root):
            rel = os.path.relpath(dirpath, self.root).replace('\\', '/')
            rel = '' if rel == '.' else rel + '/'
            dirnames[:] = [d for d in dirnames if rel + d != 'lost+found']
            have_dirs.update(rel + d for d in dirnames)
            have_files.update(rel + n for n in filenames)
        for p in sorted(have_files ^ set(self.files)):
            print(f'[{label}] file {"extra" if p in have_files else "missing"}: {p}')
            errors += 1
        for p in sorted(have_dirs ^ self.dirs):
            print(f'[{label}] dir {"extra" if p in have_dirs else "missing"}: {p}')
            errors += 1
        return errors

    def manifest(self, path):
        with open(path, 'wb') as f:
            for d in sorted(self.dirs):
                f.write(b'D\t/' + d.encode() + b'\n')
            for rel, data in sorted(self.files.items()):
                f.write(b'F\t/' + rel.encode() + b'\t' + hashlib.sha256(data).hexdigest().encode() +
                        b'\t' + str(len(data)).encode() + b'\n')


def expect_error(what, fn, exc=OSError):
    try:
        fn()
    except exc:
        return 0
    print(f'FAIL: {what} did not fail')
    return 1


def fixed_cases(m, rnd):
    errors = 0
    w = m.win

    # directories, Unicode names, names that Linux allows but are unusual on Windows
    for d in ['docs', 'docs/sub', 'docs/sub/deep', 'Ümlaute äöü ß', 'emoji 😀', 'case', 'Case']:
        os.mkdir(w(d))
        m.dirs.add(d)

    # files of awkward sizes
    for size in [0, 1, 511, 4095, 4096, 4097, 65536, 1048579, 5 * 1048576 + 17]:
        rel = f'docs/size-{size}.bin'
        data = rnd.randbytes(size)
        with open(w(rel), 'wb') as f:
            f.write(data)
        m.files[rel] = data

    # append, partial overwrite, grow with a gap, shrink
    rel = 'docs/edit.txt'
    data = b'hello world\n' * 1000
    with open(w(rel), 'wb') as f:
        f.write(data)
    with open(w(rel), 'ab') as f:
        f.write(b'appended')
    data += b'appended'
    with open(w(rel), 'r+b') as f:
        f.seek(5000)
        f.write(b'XXXXXXXXXX')
    data = data[:5000] + b'XXXXXXXXXX' + data[5010:]
    with open(w(rel), 'r+b') as f:
        f.seek(200000)
        f.write(b'after the gap')
    data = data + b'\0' * (200000 - len(data)) + b'after the gap'
    with open(w(rel), 'r+b') as f:
        f.truncate(7000)
    data = data[:7000]
    os.truncate(w(rel), 9000)  # extend with zeros
    data = data + b'\0' * 2000
    m.files[rel] = data

    # overwrite an existing file (CREATE_ALWAYS)
    with open(w('docs/size-4096.bin'), 'wb') as f:
        f.write(b'replaced')
    m.files['docs/size-4096.bin'] = b'replaced'

    # rename, move between directories, replace an existing file
    os.rename(w('docs/size-1.bin'), w('docs/renamed.bin'))
    m.files['docs/renamed.bin'] = m.files.pop('docs/size-1.bin')
    os.rename(w('docs/renamed.bin'), w('docs/sub/deep/moved.bin'))
    m.files['docs/sub/deep/moved.bin'] = m.files.pop('docs/renamed.bin')
    os.replace(w('docs/size-511.bin'), w('docs/size-0.bin'))
    m.files['docs/size-0.bin'] = m.files.pop('docs/size-511.bin')
    errors += expect_error('rename onto existing file',
                           lambda: os.rename(w('docs/size-4095.bin'), w('docs/size-4097.bin')))

    # rename a directory with contents
    os.rename(w('docs/sub'), w('docs/sub2'))
    for d in sorted(m.dirs):
        if d == 'docs/sub' or d.startswith('docs/sub/'):
            m.dirs.discard(d)
            m.dirs.add('docs/sub2' + d[len('docs/sub'):])
    for f in list(m.files):
        if f.startswith('docs/sub/'):
            m.files['docs/sub2/' + f[len('docs/sub/'):]] = m.files.pop(f)

    # case-sensitive names live side by side
    for name in ['case/file.txt', 'case/FILE.txt', 'Case/file.txt']:
        with open(w(name), 'wb') as f:
            f.write(name.encode())
        m.files[name] = name.encode()

    # Unicode file names
    for name in ['Ümlaute äöü ß/Größe.txt', 'emoji 😀/🎉.txt']:
        with open(w(name), 'wb') as f:
            f.write(name.encode())
        m.files[name] = name.encode()

    # copy with the Windows copy engine (CopyFileW keeps the timestamp)
    src = w('docs/size-1048579.bin')
    old = os.path.getmtime(src)
    os.utime(src, (old, 1600000000.5))
    if not ctypes.windll.kernel32.CopyFileW(src, w('docs/copied.bin'), True):
        print('FAIL: CopyFileW', ctypes.GetLastError())
        errors += 1
    m.files['docs/copied.bin'] = m.files['docs/size-1048579.bin']
    shutil.copy2(w('docs/size-65536.bin'), w('Case/copy2.bin'))
    m.files['Case/copy2.bin'] = m.files['docs/size-65536.bin']

    # many files in one directory (directory index grows)
    os.mkdir(w('many'))
    m.dirs.add('many')
    for i in range(600):
        rel = f'many/file-{i:04d}-{"x" * (i % 40)}.txt'
        data = str(i).encode() * (i % 7)
        with open(w(rel), 'wb') as f:
            f.write(data)
        m.files[rel] = data
    for i in range(0, 600, 3):  # delete every third
        rel = f'many/file-{i:04d}-{"x" * (i % 40)}.txt'
        os.remove(w(rel))
        del m.files[rel]

    # delete: file, empty dir, non-empty dir fails, whole tree
    os.remove(w('docs/size-4097.bin'))
    del m.files['docs/size-4097.bin']
    os.mkdir(w('empty'))
    os.rmdir(w('empty'))
    errors += expect_error('rmdir of non-empty directory', lambda: os.rmdir(w('docs')))
    os.makedirs(w('tree/a/b/c'))
    for i in range(20):
        with open(w(f'tree/a/b/c/{i}.txt'), 'wb') as f:
            f.write(b'x' * i)
    shutil.rmtree(w('tree'))

    errors += expect_error('open missing file', lambda: open(w('nope/missing.txt'), 'rb'))
    return errors


def random_ops(m, rnd, ops):
    w = m.win
    os.mkdir(w('rnd'))
    m.dirs.add('rnd')
    for i in range(ops):
        dirs = sorted(d for d in m.dirs if d == 'rnd' or d.startswith('rnd/'))
        files = sorted(f for f in m.files if f.startswith('rnd/'))
        op = rnd.random()
        if op < 0.30 or not files:
            rel = f'{rnd.choice(dirs)}/f{i}.dat'
            data = rnd.randbytes(rnd.choice([0, 10, 3000, 70000, 300000]))
            with open(w(rel), 'wb') as f:
                f.write(data)
            m.files[rel] = data
        elif op < 0.45:
            rel = rnd.choice(files)
            data = m.files[rel]
            off = rnd.randrange(0, len(data) + 5000)
            chunk = rnd.randbytes(rnd.choice([1, 100, 5000, 100000]))
            with open(w(rel), 'r+b') as f:
                f.seek(off)
                f.write(chunk)
            if off > len(data):
                data = data + b'\0' * (off - len(data))
            m.files[rel] = data[:off] + chunk + data[off + len(chunk):]
        elif op < 0.55:
            rel = rnd.choice(files)
            size = rnd.randrange(0, len(m.files[rel]) * 2 + 10)
            os.truncate(w(rel), size)
            d = m.files[rel]
            m.files[rel] = d[:size] + b'\0' * max(0, size - len(d))
        elif op < 0.65:
            rel = rnd.choice(files)
            os.remove(w(rel))
            del m.files[rel]
        elif op < 0.75:
            rel = rnd.choice(files)
            new = f'{rnd.choice(dirs)}/r{i}.dat'
            os.rename(w(rel), w(new))
            m.files[new] = m.files.pop(rel)
        elif op < 0.85 and len(dirs) < 30:
            rel = f'{rnd.choice(dirs)}/d{i}'
            os.mkdir(w(rel))
            m.dirs.add(rel)
        else:
            empty = [d for d in dirs if d != 'rnd' and
                     not any(x.startswith(d + '/') for x in list(m.files) + list(m.dirs))]
            if empty:
                d = rnd.choice(empty)
                os.rmdir(w(d))
                m.dirs.discard(d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('image')
    ap.add_argument('drive')
    ap.add_argument('manifest')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--ops', type=int, default=400)
    ap.add_argument('--crash', action='store_true')
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    m = Model(a.drive + '\\')

    p = mount(a.exe, a.image, a.drive, write=True)
    errors = 0
    if a.crash:
        try:
            fixed_cases(m, rnd)
            random_ops(m, rnd, a.ops)
            time.sleep(4)  # background flush interval is 2 s
        finally:
            p.kill()  # TerminateProcess: no unmount, no final flush
            p.wait()
            wait_for_drive(a.drive, p, present=False)
        m.manifest(a.manifest)
        print(f'killed with {len(m.files)} files, {len(m.dirs)} dirs written')
        return
    try:
        t = time.time()
        big = rnd.randbytes(1048576)
        with open(m.win('big.bin'), 'wb') as f:
            for _ in range(64):
                f.write(big)
            f.flush()
            os.fsync(f.fileno())  # until the data is on the (image) disk
        m.files['big.bin'] = big * 64
        dt = time.time() - t
        print(f'64 MiB written in {dt:.2f} s ({64 / dt:.0f} MiB/s)')
        errors += fixed_cases(m, rnd)
        random_ops(m, rnd, a.ops)
        errors += m.check_all('mounted')
        copied_mtime = os.path.getmtime(m.win('docs/copied.bin'))
    finally:
        unmount(p, a.drive)

    # Everything must still be there after a clean unmount.
    p = mount(a.exe, a.image, a.drive, write=False)
    try:
        errors += m.check_all('remounted')
        if abs(os.path.getmtime(m.win('docs/copied.bin')) - 1600000000.5) > 0.01:
            print('FAIL: CopyFileW did not keep the modification time:',
                  os.path.getmtime(m.win('docs/copied.bin')), copied_mtime)
            errors += 1
        errors += expect_error('writing on a read-only mount',
                               lambda: open(m.win('ro.txt'), 'wb'))
    finally:
        unmount(p, a.drive)

    m.manifest(a.manifest)
    print(f'{len(m.files)} files, {len(m.dirs)} dirs, {errors} problems')
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
