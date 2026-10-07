"""ホストOSのshell構文に依存せず、同梱MSYS2でNocturneを構築する。"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-j', '--jobs', type=int, default=8)
    parser.add_argument('--log', help='UTF-8ツール出力の保存先')
    parser.add_argument('targets', nargs='*', default=['all'])
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('並列数は1以上を指定してください')
    root = Path(__file__).resolve().parents[1]
    bash = root / 'tools/msys64/usr/bin/bash.exe'
    env = dict(os.environ, MSYSTEM='UCRT64', CHERE_INVOKING='1', NOCTURNE_BUILD_ROOT=root.as_posix())
    # Targets remain individual argv entries, never shell-source interpolation.
    command = [str(bash), '-lc', 'cd "$NOCTURNE_BUILD_ROOT" && exec make "$@"',
               'nocturne-build', '-j'+str(args.jobs), *(args.targets or ['all'])]
    if args.log:
        log = Path(args.log)
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open('wb') as out:
            return subprocess.run(command, cwd=root, env=env, stdout=out, stderr=subprocess.STDOUT).returncode
    return subprocess.run(command, cwd=root, env=env).returncode

if __name__ == '__main__':
    sys.exit(main())
