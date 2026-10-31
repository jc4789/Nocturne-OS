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
    # 作業木（git worktree）には無視されたフォルダー（tools/・build/）が無い。道具は本体のものを使い、
    # 出力は本体の build/wt/<作業木の名>/ に置く。make の BUILD は空白を含まない相対の道で渡す。
    main_root = root
    common = subprocess.run(['git', 'rev-parse', '--path-format=absolute', '--git-common-dir'],
                            cwd=root, capture_output=True, text=True)
    if common.returncode == 0 and common.stdout.strip():
        main_root = Path(common.stdout.strip()).resolve().parent
    make_vars = []
    if main_root != root:
        out = main_root / 'build' / 'wt' / root.name
        make_vars.append('BUILD=' + Path(os.path.relpath(out, root)).as_posix())
    bash = main_root / 'tools/msys64/usr/bin/bash.exe'
    # ログイン時の既定PATHはWindows側のRustなどを落とす。
    # 継承してもMSYS2はUCRT64と/usr/binを先頭に置くため、同梱ツールを優先できる。
    env = dict(os.environ, MSYSTEM='UCRT64', CHERE_INVOKING='1',
               MSYS2_PATH_TYPE='inherit', NOCTURNE_BUILD_ROOT=root.as_posix())
    # Targets remain individual argv entries, never shell-source interpolation.
    command = [str(bash), '-lc', 'cd "$NOCTURNE_BUILD_ROOT" && exec make "$@"',
               'nocturne-build', '-j'+str(args.jobs), *make_vars, *(args.targets or ['all'])]
    if args.log:
        log = Path(args.log)
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open('wb') as out:
            return subprocess.run(command, cwd=root, env=env, stdout=out, stderr=subprocess.STDOUT).returncode
    return subprocess.run(command, cwd=root, env=env).returncode

if __name__ == '__main__':
    sys.exit(main())
