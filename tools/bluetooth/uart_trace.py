#!/usr/bin/env python3
"""Extract logged UART chunks from an existing Android hsuart trace.

This is an offline reader, not a packet decoder or a device trace configurator.
The vendor logger truncates many dumps: preserve the reported chunk length,
captured prefix and source line independently. A DMA chunk is not necessarily
one protocol frame, so never join chunks or fill missing bytes here.
"""
import argparse
import json
from pathlib import Path
import re

CHUNK = re.compile(
    r'\s(?P<timestamp>\d+\.\d+): serial_info: (?P<device>\S+): '
    r'dump_ipc : DMA (?P<direction>Tx|Rx)\['
    r'[^:\]]+:(?P<length>\d+)\] :(?P<data>.*)$')
BAUD = re.compile(
    r'\s(?P<timestamp>\d+\.\d+): serial_info: (?P<device>\S+): '
    r'msm_geni_serial_set_termios: baud (?P<baud>\d+)\s*$')


def extract(path, device):
    """Return trace events in file order, with original one-based line numbers."""
    events = []
    for number, line in enumerate(path.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
        match = CHUNK.search(line)
        if match and match['device'] == device:
            values = match['data'].split()
            if not all(re.fullmatch(r'[0-9a-fA-F]{2}', value) for value in values):
                raise ValueError(f'non-hex UART dump at line {number}')
            data = bytes.fromhex(' '.join(values))
            reported = int(match['length'])
            if len(data) > reported:
                raise ValueError(f'UART dump exceeds reported chunk length at line {number}')
            events.append(dict(line=number, timestamp=match['timestamp'],
                               device=device, event='uart_chunk', direction=match['direction'],
                               reported_bytes=reported, captured_bytes=len(data),
                               complete_chunk=len(data) == reported, hex=data.hex(' ')))
            continue
        match = BAUD.search(line)
        if match and match['device'] == device:
            events.append(dict(line=number, timestamp=match['timestamp'], device=device,
                               event='baud', baud=int(match['baud'])))
    return events


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--device', default='1994000.qcom,qup_uart')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.out.resolve() == args.trace.resolve():
        parser.error('output must not overwrite the original trace')
    events = extract(args.trace, args.device)
    if not events:
        raise ValueError('no matching UART chunks or baud events in the trace')
    chunks = [event for event in events if event['event'] == 'uart_chunk']
    record = dict(source=str(args.trace.resolve()), device=args.device,
                  note='DMA chunks and logged prefixes only; not reconstructed protocol frames',
                  chunk_count=len(chunks),
                  truncated_chunks=sum(not chunk['complete_chunk'] for chunk in chunks),
                  events=events)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(f'Extracted {len(chunks)} UART chunks; {record["truncated_chunks"]} have truncated prefixes.')
    print(f'Saved {args.out}; original trace unchanged.')


if __name__ == '__main__':
    main()
