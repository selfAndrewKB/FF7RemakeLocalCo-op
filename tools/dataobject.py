"""Decodes FF7 Remake (UE 4.18) DataObject tables (DataObject/**/*.uasset + .uexp) into rows. Read-only.

  python -I dataobject.py <table.uasset> [column ...]     prints every row (all columns, or the named ones) as TSV

Layout (read from the files): the .uexp starts with 10 bytes, then
int32 rows, int32 columns, then per column an FName (int64 into the package's name map) and a type byte; then per row
its FName and each column's value. A column named *_Array is an int32 count and that many values. Types seen:
0x02 u8, 0x07 int32, 0x0a FString, 0x0b FName; others are added as tables need them (an unknown type stops the read).
"""
import pathlib, struct, sys

SCALARS = {0x01: '<?', 0x02: '<B', 0x03: '<b', 0x04: '<h', 0x05: '<H', 0x07: '<i', 0x08: '<I', 0x09: '<f'}


def name_map(asset):
    count, offset = struct.unpack_from('<ii', asset, 0x29)
    names, pos = [], offset
    for _ in range(count):
        n = struct.unpack_from('<i', asset, pos)[0]
        pos += 4
        if n < 0:
            names.append(asset[pos:pos - n * 2 - 2].decode('utf-16-le'))
            pos += -n * 2 + 4
        else:
            names.append(asset[pos:pos + n - 1].decode('latin-1'))
            pos += n + 4
    return names


def load(path):
    path = pathlib.Path(path)
    names = name_map(path.read_bytes())
    data = path.with_suffix('.uexp').read_bytes()
    pos = [10]

    def take(fmt):
        v = struct.unpack_from(fmt, data, pos[0])[0]
        pos[0] += struct.calcsize(fmt)
        return v

    def fname():
        index, number = take('<i'), take('<i')  # UE stores name_N as (index, N + 1)
        return names[index] + ('' if number == 0 else f'_{number - 1}')

    def value(kind):
        if kind in SCALARS:
            return take(SCALARS[kind])
        if kind == 0x0b:
            return fname()
        if kind == 0x0a:
            n = take('<i')
            if n == 0:
                return ''
            if n < 0:
                s = data[pos[0]:pos[0] - n * 2 - 2].decode('utf-16-le')
                pos[0] += -n * 2
                return s
            s = data[pos[0]:pos[0] + n - 1].decode('latin-1')
            pos[0] += n
            return s
        raise ValueError(f'unknown column type {kind:#x} at {pos[0]:#x}')

    rows, cols = take('<i'), take('<i')
    columns = []
    for _ in range(cols):
        nm = fname()
        columns.append((nm, take('<B')))
    table = []
    for _ in range(rows):
        row = {'Name': fname()}
        for nm, kind in columns:
            row[nm] = [value(kind) for _ in range(take('<i'))] if nm.endswith('_Array') else value(kind)
        table.append(row)
    return columns, table, len(data) - pos[0]


def main():
    sys.stdout.reconfigure(encoding='utf-8')  # FString cells can be Japanese; the Windows console default is cp1252
    columns, table, left = load(sys.argv[1])
    wanted = sys.argv[2:] or [c for c, _ in columns]
    print('\t'.join(['Name'] + wanted))
    for row in table:
        print('\t'.join([row['Name']] + [str(row[c]) for c in wanted]))
    print(f'# {len(table)} rows, {len(columns)} columns, {left} bytes after the last row', file=sys.stderr)


if __name__ == '__main__':
    main()
