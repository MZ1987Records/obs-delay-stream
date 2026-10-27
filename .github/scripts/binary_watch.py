#!/usr/bin/env python3
"""cloudflared と FFmpeg の承認版（src/core/approved-binaries.hpp）を上流と比べる。

.github/workflows/binary-watch.yml から呼ぶ。標準ライブラリだけを使い、`python3 -I` で実行する。

  check     上流を確かめ、更新候補と警告を JSON に書く。候補の取得物はハッシュを計算するだけで実行しない。
  apply     check の結果のうち 1 件を承認一覧と docs/dependencies.md に書き込む。
  selftest  取得なしで、解析・候補の選び方・書き換えを確かめる。
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import sys
import traceback
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

HPP = 'src/core/approved-binaries.hpp'
DEPS = 'docs/dependencies.md'
FIELDS = ('name', 'version', 'url', 'download_size', 'download_sha256', 'exe_in_archive', 'exe_size', 'exe_sha256')
BLOCK_RE = re.compile(r'((?:\t//[^\n]*\n)+)\tinline constexpr ApprovedBinary (\w+) = \{\n(.*?)\n\t\};', re.S)
TOKEN_RE = re.compile(r'"((?:[^"\\]|\\.)*)"|(\d+)|(nullptr)')

CF_REPO = 'cloudflare/cloudflared'
CF_ASSET = 'cloudflared-windows-amd64.exe'
CF_VERSION_RE = re.compile(r'\d{4}\.\d{1,2}\.\d{1,3}')
CF_DATE_RE = re.compile(r'（(\d{4}-\d{2}-\d{2}) 公開）')
FF_REPO = 'BtbN/FFmpeg-Builds'
FF_TAG_RE = re.compile(r'autobuild-(\d{4})-(\d{2})-\d{2}-\d{2}-\d{2}')
# win64 の LGPL 静的ビルド。shared・GPL・master（N-xxxxx）・latest は一致しない。
FF_ASSET_RE = re.compile(r'ffmpeg-(n(\d+)\.(\d+)(?:\.\d+)?(?:-\d+-g([0-9a-f]+))?)-win64-lgpl-(\d+)\.(\d+)\.zip')
SHA_RE = re.compile(r'[0-9a-f]{64}')
DATE_RE = re.compile(r'\d{4}-\d{2}-\d{2}')


# ---- 承認一覧の解析と書き換え ----

def parse_approved(text):
    """ApprovedBinary の定義を識別子ごとに返す。隣り合う文字列リテラルは連結する。"""
    out = {}
    for m in BLOCK_RE.finditer(text):
        body = re.sub(r'"\s*\n\s*"', '', m.group(3))
        values = []
        for t in TOKEN_RE.finditer(body):
            if t.group(1) is not None:
                values.append(t.group(1).replace('\\\\', '\\'))
            elif t.group(2):
                values.append(int(t.group(2)))
            else:
                values.append(None)
        if len(values) != len(FIELDS):
            raise ValueError(f'{m.group(2)}: {len(values)} fields, expected {len(FIELDS)}')
        d = dict(zip(FIELDS, values))
        d['comment'] = m.group(1)
        d['span'] = m.span()
        out[m.group(2)] = d
    return out


def replace_tokens(text, pairs):
    """旧値を新値へ置き換える。英数字の途中（ハッシュの一部など）には一致させない。"""
    table = {str(o): str(n) for o, n in pairs if str(o) != str(n)}
    if not table:
        return text
    alt = '|'.join(map(re.escape, sorted(table, key=len, reverse=True)))
    return re.sub(rf'(?<![0-9A-Za-z])({alt})(?![0-9A-Za-z])', lambda m: table[m.group(1)], text)


def md_pairs(pairs):
    # docs/dependencies.md ではバイト数を 3 桁区切りで書いている。
    return [(f'{o:,}', f'{n:,}') if isinstance(o, int) else (o, n) for o, n in pairs]


def apply_update(hpp_text, deps_text, upd):
    """更新候補を承認一覧の該当ブロックと docs/dependencies.md の該当行だけに書き込む。"""
    block = parse_approved(hpp_text)[upd['id']]
    s, e = block['span']
    new_hpp = hpp_text[:s] + replace_tokens(hpp_text[s:e], upd['pairs']) + hpp_text[e:]
    got = parse_approved(new_hpp)[upd['id']]
    wrong = {k: got[k] for k, v in upd['expected'].items() if got[k] != v}
    if wrong:
        raise ValueError(f'{upd["id"]} was not rewritten as expected: {wrong}')

    lines = deps_text.split('\n')
    rows = [i for i, line in enumerate(lines) if line.startswith(f'| {block["name"]} |')]
    if len(rows) != 1:
        raise ValueError(f'{DEPS}: expected one row for {block["name"]}, found {len(rows)}')
    pairs = md_pairs(upd['pairs'])
    row = replace_tokens(lines[rows[0]], pairs)
    left = [o for o, n in pairs if o != n and replace_tokens(row, [(o, '\0')]) != row]
    if row == lines[rows[0]] or left:
        raise ValueError(f'{DEPS}: row for {block["name"]} was not rewritten (left: {left})')
    lines[rows[0]] = row
    return new_hpp, '\n'.join(lines)


# ---- 候補の選び方 ----

def version_key(v):
    return tuple(int(x) for x in v.split('.'))


def latest_month_end(tags, today):
    """当月より前の autobuild のうち最も新しいもの（＝前月の月末ビルド）。

    BtbN の日次ビルドは約 2 週間で消え、月末ビルドは約 2 年保持されるため、月末ビルドだけを候補にする。
    """
    cur = f'{today:%Y-%m}'
    past = [t for t in tags if (m := FF_TAG_RE.fullmatch(t)) and f'{m[1]}-{m[2]}' < cur]
    return max(past, default=None)


def lgpl_static_builds(names):
    """win64 LGPL 静的ビルドの ZIP を系統 (major, minor) ごとに (資産名, 版) で返す。"""
    out = {}
    for n in names:
        m = FF_ASSET_RE.fullmatch(n)
        if m and (m[2], m[3]) == (m[5], m[6]):
            out[(int(m[5]), int(m[6]))] = (n, m[1])
    return out


def ffmpeg_ref(version):
    # git describe 形式ならコミット、タグちょうどの版ならタグを FFmpeg 本体の比較に使う。
    m = FF_ASSET_RE.fullmatch(f'ffmpeg-{version}-win64-lgpl-0.0.zip')
    return m[4] if m and m[4] else version


def verify_download(size, sha, api_size, refs):
    """取得物を上流の記載と照合する。問題があれば理由を、なければ None を返す。"""
    if size != api_size:
        return f'サイズ {size:,} bytes が GitHub API の記載 {api_size:,} bytes と違う'
    known = {k: v for k, v in refs.items() if v}
    if not known:
        return '照合できる SHA-256 が上流にない（GitHub API の digest もリリース本文の記載もない）'
    bad = [k for k, v in known.items() if v != sha]
    if bad:
        return f'SHA-256 `{sha}` が {"・".join(bad)} と違う'
    return None


# ---- 上流との通信 ----

def api(path):
    req = urllib.request.Request(f'https://api.github.com/{path}', headers={
        'Accept': 'application/vnd.github+json',
        'X-GitHub-Api-Version': '2022-11-28',
        'User-Agent': 'obs-delay-stream-binary-watch',
    })
    if os.environ.get('GITHUB_TOKEN'):
        req.add_header('Authorization', f'Bearer {os.environ["GITHUB_TOKEN"]}')
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


def download(url, dest):
    """url を dest へ保存し、(バイト数, SHA-256) を返す。保存したものは実行しない。"""
    dest.parent.mkdir(parents=True, exist_ok=True)
    h, size = hashlib.sha256(), 0
    req = urllib.request.Request(url, headers={'User-Agent': 'obs-delay-stream-binary-watch'})
    with urllib.request.urlopen(req, timeout=600) as r, open(dest, 'wb') as f:
        while chunk := r.read(1 << 20):
            h.update(chunk)
            f.write(chunk)
            size += len(chunk)
    return size, h.hexdigest()


def hash_zip_entry(path, entry):
    """ZIP の 1 エントリを展開せずに読み、(バイト数, SHA-256) を返す。無ければ KeyError。"""
    h, size = hashlib.sha256(), 0
    with zipfile.ZipFile(path) as z, z.open(entry) as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
            size += len(chunk)
    return size, h.hexdigest()


def find_asset(release, name):
    return next((a for a in (release or {}).get('assets', []) if a['name'] == name), None)


def digest_of(asset):
    d = (asset or {}).get('digest') or ''
    return d.removeprefix('sha256:') if SHA_RE.fullmatch(d.removeprefix('sha256:')) else None


# ---- 報告文 ----

def run_url():
    env = os.environ
    if 'GITHUB_RUN_ID' not in env:
        return 'ローカル実行'
    return f'{env["GITHUB_SERVER_URL"]}/{env["GITHUB_REPOSITORY"]}/actions/runs/{env["GITHUB_RUN_ID"]}'


def alert(title, body):
    return {'title': f'binary-watch: {title}', 'body': f'{body}\n\n検知した実行: {run_url()}\n'}


def check_approved(repo, tag, asset_name, old, alerts):
    """承認版が上流にまだあり、記載が承認値と同じかを確かめる。消えていれば警告文を返す。"""
    label = f'{old["name"]} {old["version"]}'
    asset = find_asset(api(f'repos/{repo}/releases/tags/{tag}'), asset_name)
    if asset is None:
        body = (f'承認版の取得元 {old["url"]} が上流のリリースから消えている。'
            'Auto モードで新しく取得する利用者は使えなくなる（取得済みで照合に通る exe はそのまま使える）。\n\n'
            'binary-watch が更新候補の PR を作っていればそれを確かめて承認する。'
            '候補が無ければ手動で新しい版を選び、承認一覧と docs/dependencies.md を更新する。')
        alerts.append(alert(f'承認版の {label} が上流から取得できない', body))
        return body
    digest = digest_of(asset)
    if asset['size'] != old['download_size'] or (digest and digest != old['download_sha256']):
        alerts.append(alert(
            f'承認版の {label} の上流の記載が承認値と違う',
            f'上流の {asset_name} は {asset["size"]:,} bytes、digest `{digest}`。'
            f'承認値は {old["download_size"]:,} bytes、`{old["download_sha256"]}`。'
            '上流で取得物が差し替えられた可能性がある。告知を確かめ、必要なら公開停止を判断する（docs/security-hardening-plan.md 10 章）。'))
    return None


def make_update(name, ident, old_label, new_label, branch, pairs, expected, rows, links, checks, gone):
    """apply と PR の作成に使う 1 件分の情報を作る。"""
    repo = os.environ.get('GITHUB_REPOSITORY', 'OWNER/REPO')
    lines = [f'{name} の上流で新しい版が出たか承認版が消えたため、承認一覧 `{HPP}` と `{DEPS}` を書き換えた。'
        'binary-watch ワークフローが自動で作った PR で、取得物のサイズと SHA-256 を計算しただけで、実行も動作確認もしていない。'
        '承認・マージ・リリースは人が行う。', '']
    if gone:
        lines += ['> [!CAUTION]', f'> 承認版 {old_label} は上流から取得できなくなっている。Auto モードで新しく取得する利用者が使えないため、早めに承認・リリースする。', '']
    lines += ['| 項目 | 旧（承認版） | 新（候補） |', '|---|---|---|']
    lines += [f'| {k} | {o} | {n} |' for k, o, n in rows]
    lines += [''] + [f'- {x}' for x in links] + ['', '## 承認前に確かめること', '']
    lines += [f'- [ ] {x}' for x in checks]
    lines += ['', '## CI について', '',
        f'GITHUB_TOKEN で作った PR では `pull_request` の build が動かない。代わりに build を workflow_dispatch で `{branch}` に対して起動した'
        f'（[実行一覧](https://github.com/{repo}/actions/workflows/build.yml?query=branch%3A{branch})）。'
        'PR のチェック欄に結果を出したいときは、PR を一度閉じて開き直す（人の操作なので `pull_request` の build が動く）。',
        '', '同じ候補の PR は、閉じたものも含めて一度しか作らない。却下するときはこの PR を閉じればよい。']
    return {
        'name': name, 'id': ident, 'branch': branch,
        'title': f'{name} の承認版を {old_label} から {new_label} に更新する（自動検知）',
        'commit_message': f'chore(deps): {name} の承認版を {new_label} に更新する\n\nbinary-watch ワークフローが上流の変化を検知して書き換えた。',
        'pairs': pairs, 'expected': expected, 'body': '\n'.join(lines) + '\n',
    }


def check_cloudflared(approved, dl, today, updates, alerts, summary):
    old = approved['kApprovedCloudflared']
    gone = check_approved(CF_REPO, old['version'], CF_ASSET, old, alerts)
    latest = api(f'repos/{CF_REPO}/releases/latest')
    ver = latest['tag_name']
    if not CF_VERSION_RE.fullmatch(ver):
        raise ValueError(f'unexpected cloudflared tag: {ver!r}')
    if version_key(ver) <= version_key(old['version']):
        summary.append(f'- cloudflared: 承認版 {old["version"]} のまま（上流の最新 {ver}）{"、承認版が上流から消えている" if gone else ""}')
        return
    asset = find_asset(latest, CF_ASSET)
    if asset is None:
        alerts.append(alert(f'cloudflared {ver} に {CF_ASSET} がない', f'上流の最新 {ver} に Windows amd64 の exe が無い。リリースを確かめる。'))
        summary.append(f'- cloudflared: {ver} に {CF_ASSET} が無い')
        return
    url = f'https://github.com/{CF_REPO}/releases/download/{ver}/{CF_ASSET}'
    size, sha = download(url, dl / 'cloudflared' / ver / CF_ASSET)
    m = re.search(rf'^{re.escape(CF_ASSET)}:\s*([0-9a-f]{{64}})\s*$', latest.get('body') or '', re.M)
    refs = {'GitHub API の digest': digest_of(asset), 'リリース本文のチェックサム': m and m[1]}
    problem = verify_download(size, sha, asset['size'], refs)
    if problem:
        alerts.append(alert(f'cloudflared {ver} の取得物が上流の記載と一致しない', f'{url}: {problem}。PR は作らない。'))
        summary.append(f'- cloudflared: {ver} の照合に失敗（{problem}）')
        return
    date = latest['published_at'][:10]
    if not DATE_RE.fullmatch(date):
        raise ValueError(f'unexpected published_at: {date!r}')
    pairs = [(old['version'], ver), (old['download_size'], size), (old['download_sha256'], sha)]
    old_date = CF_DATE_RE.search(old['comment'])
    if old_date:
        pairs.append((old_date[1], date))
    expected = {'version': ver, 'url': url, 'download_size': size, 'download_sha256': sha, 'exe_size': size, 'exe_sha256': sha}
    matched = '・'.join(k for k, v in refs.items() if v)
    rows = [
        ('版', f'`{old["version"]}`' + (f'（{old_date[1]} 公開）' if old_date else ''), f'`{ver}`（{date} 公開）'),
        ('取得 URL', old['url'], url),
        ('サイズ', f'{old["download_size"]:,} bytes', f'{size:,} bytes'),
        ('SHA-256', f'`{old["download_sha256"]}`', f'`{sha}`'),
    ]
    links = [f'リリースノート: https://github.com/{CF_REPO}/releases/tag/{ver}',
        f'変更差分: https://github.com/{CF_REPO}/compare/{old["version"]}...{ver}',
        f'SHA-256 の照合: {matched}と一致']
    checks = ['リリースノートと Cloudflare の告知を読み、採用してよい版か判断する',
        'SHA-256 を取得元と別の経路でも照合する',
        'build ワークフローが通る（下記）',
        '実機で Quick Tunnel の公開と停止ができる',
        f'`{DEPS}` の確認日を更新する']
    updates.append(make_update('cloudflared', 'kApprovedCloudflared', old['version'], ver, f'binary-watch/cloudflared-{ver}',
        pairs, expected, rows, links, checks, gone))
    summary.append(f'- cloudflared: {old["version"]} → {ver} の候補を作った')


def check_ffmpeg(approved, dl, today, updates, alerts, summary):
    old = approved['kApprovedFfmpeg']
    m = re.fullmatch(rf'https://github\.com/{FF_REPO}/releases/download/([^/]+)/([^/]+)', old['url'])
    old_tag, old_asset = m[1], m[2]
    om = FF_ASSET_RE.fullmatch(old_asset)
    branch = (int(om[5]), int(om[6]))
    gone = check_approved(FF_REPO, old_tag, old_asset, old, alerts)

    rels = api(f'repos/{FF_REPO}/releases?per_page=100')
    tag = latest_month_end([r['tag_name'] for r in rels], today)
    if tag is None:
        raise ValueError('no month-end autobuild found')
    rel = next(r for r in rels if r['tag_name'] == tag)
    builds = lgpl_static_builds(a['name'] for a in rel['assets'])
    for br in sorted(b for b in builds if b > branch):
        alerts.append(alert(
            f'BtbN に FFmpeg {br[0]}.{br[1]} 系の LGPL 静的ビルドが出た（自動では採らない）',
            f'月末ビルド {tag} に {builds[br][0]} がある。承認版は {branch[0]}.{branch[1]} 系。'
            '系統を変えるかは、構成・ライセンス・RTSP E2E 計測の動作を確かめて人が判断する。'
            f'\n\nhttps://github.com/{FF_REPO}/releases/tag/{tag}'))
    label = f'{branch[0]}.{branch[1]} 系 LGPL'
    if tag <= old_tag:
        summary.append(f'- FFmpeg: 承認版 {old["version"]}（{old_tag}）のまま。最新の月末ビルドは {tag}{"、承認版が上流から消えている" if gone else ""}')
        return
    if branch not in builds:
        alerts.append(alert(f'BtbN の月末ビルド {tag} に FFmpeg {label} 静的ビルドがない',
            f'承認版と同じ系統・ライセンスの win64 静的ビルドが無い。系統やライセンスの区分が変わった可能性がある。'
            f'\n\nhttps://github.com/{FF_REPO}/releases/tag/{tag}'))
        summary.append(f'- FFmpeg: 月末ビルド {tag} に {label} が無い')
        return
    asset_name, ver = builds[branch]
    # 承認版が消えていれば、同じ版でも保持される月末ビルドへ移す。
    if ver == old['version'] and not gone:
        summary.append(f'- FFmpeg: 月末ビルド {tag} も {ver} のまま（承認版と同じ）')
        return
    url = f'https://github.com/{FF_REPO}/releases/download/{tag}/{asset_name}'
    zip_path = dl / 'ffmpeg' / tag / asset_name
    size, sha = download(url, zip_path)
    asset = find_asset(rel, asset_name)
    problem = verify_download(size, sha, asset['size'], {'GitHub API の digest': digest_of(asset)})
    stem = asset_name.removesuffix('.zip')
    if not problem:
        try:
            exe_size, exe_sha = hash_zip_entry(zip_path, f'{stem}/bin/ffmpeg.exe')
        except KeyError:
            problem = f'ZIP に {stem}/bin/ffmpeg.exe が無い'
    if problem:
        alerts.append(alert(f'FFmpeg {ver} の取得物が上流の記載と一致しない', f'{url}: {problem}。PR は作らない。'))
        summary.append(f'- FFmpeg: {ver} の照合に失敗（{problem}）')
        return
    pairs = [(old['version'], ver), (old_tag, tag), (old['download_size'], size), (old['download_sha256'], sha),
        (old['exe_size'], exe_size), (old['exe_sha256'], exe_sha)]
    expected = {'version': ver, 'url': url, 'download_size': size, 'download_sha256': sha,
        'exe_in_archive': f'{stem}\\bin\\ffmpeg.exe', 'exe_size': exe_size, 'exe_sha256': exe_sha}
    rows = [
        ('版', f'`{old["version"]}`（{old_tag}）', f'`{ver}`（{tag}）'),
        ('取得 URL', old['url'], url),
        ('ZIP のサイズ', f'{old["download_size"]:,} bytes', f'{size:,} bytes'),
        ('ZIP の SHA-256', f'`{old["download_sha256"]}`', f'`{sha}`'),
        ('`ffmpeg.exe` のサイズ', f'{old["exe_size"]:,} bytes', f'{exe_size:,} bytes'),
        ('`ffmpeg.exe` の SHA-256', f'`{old["exe_sha256"]}`', f'`{exe_sha}`'),
    ]
    links = [f'BtbN のリリース: https://github.com/{FF_REPO}/releases/tag/{tag}',
        f'FFmpeg 本体の変更差分: https://github.com/FFmpeg/FFmpeg/compare/{ffmpeg_ref(old["version"])}...{ffmpeg_ref(ver)}',
        'FFmpeg のセキュリティ情報: https://ffmpeg.org/security.html',
        'ZIP の SHA-256 の照合: GitHub API の digest と一致。`ffmpeg.exe` は ZIP から展開せずに読んで計算した',
        '候補は BtbN の月末ビルドだけ（日次ビルドは約 2 週間で消え、月末ビルドは約 2 年保持される）']
    checks = ['FFmpeg 本体の変更差分とセキュリティ情報を読み、採用してよい版か判断する',
        'ZIP の SHA-256 を取得元と別の経路でも照合する',
        'build ワークフローが通る（下記）',
        '実機で RTSP E2E 計測が動き、`ffmpeg -version` の構成が LGPL の静的ビルドのまま',
        f'`{DEPS}` の確認日を更新する']
    # 同じ版が別の月末ビルドに載ることがあるため、ブランチ名は月末ビルドのタグで決める。
    updates.append(make_update('FFmpeg', 'kApprovedFfmpeg', f'{old["version"]}（{old_tag}）', f'{ver}（{tag}）',
        f'binary-watch/ffmpeg-{tag}', pairs, expected, rows, links, checks, gone))
    summary.append(f'- FFmpeg: {old["version"]} → {ver}（{tag}）の候補を作った')


def cmd_check(args):
    approved = parse_approved(Path(args.hpp).read_text(encoding='utf-8'))
    today = datetime.datetime.now(datetime.timezone.utc).date()
    updates, alerts, summary = [], [], []
    failed = False
    for name, fn in (('cloudflared', check_cloudflared), ('FFmpeg', check_ffmpeg)):
        # 片方の失敗（上流の形式変更や通信エラー）で、もう片方の候補と警告まで失わないようにする。
        try:
            fn(approved, Path(args.download_dir), today, updates, alerts, summary)
        except Exception as e:
            traceback.print_exc()
            print(f'::error::{name} の確認に失敗した: {e!r}')
            summary.append(f'- {name}: 確認に失敗した（{e!r}）')
            failed = True
    Path(args.out).write_text(json.dumps({'updates': updates, 'alerts': alerts}, ensure_ascii=False), encoding='utf-8')
    text = '## binary-watch\n\n' + '\n'.join(summary) + '\n' + ''.join(f'- 警告: {a["title"]}\n' for a in alerts)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a', encoding='utf-8') as f:
            f.write(text)
    print(text)
    # 結果を書いたうえで失敗を返し、実行を失敗扱いにして気づけるようにする。
    return 1 if failed else 0


def cmd_apply(args):
    upd = next(u for u in json.loads(Path(args.result).read_text(encoding='utf-8'))['updates'] if u['name'] == args.name)
    hpp, deps = Path(args.hpp), Path(args.deps)
    new_hpp, new_deps = apply_update(hpp.read_text(encoding='utf-8'), deps.read_text(encoding='utf-8'), upd)
    hpp.write_text(new_hpp, encoding='utf-8', newline='\n')
    deps.write_text(new_deps, encoding='utf-8', newline='\n')


def cmd_selftest(args):
    hpp_text = Path(args.hpp).read_text(encoding='utf-8')
    deps_text = Path(args.deps).read_text(encoding='utf-8')
    approved = parse_approved(hpp_text)
    cf, ff = approved['kApprovedCloudflared'], approved['kApprovedFfmpeg']
    assert cf['exe_in_archive'] is None and SHA_RE.fullmatch(cf['exe_sha256']), cf
    # 承認値そのもの（版・タグ・系統）は直書きしない。承認版を更新しても selftest が通るようにする。
    ff_tag, ff_asset = ff['url'].split('/')[-2:]
    fm = FF_ASSET_RE.fullmatch(ff_asset)
    assert FF_TAG_RE.fullmatch(ff_tag) and fm and fm[1] == ff['version'], ff
    assert ff['exe_in_archive'] == ff_asset.removesuffix('.zip') + '\\bin\\ffmpeg.exe', ff
    ff_series = f'{fm[5]}.{fm[6]}'

    assert version_key('2026.10.10') > version_key('2026.10.3') > version_key('2026.9.30')
    tags = ['latest', 'autobuild-2026-10-24-13-02', 'autobuild-2026-09-30-13-08', 'autobuild-2026-09-29-13-00',
        'autobuild-2026-08-31-13-27']
    assert latest_month_end(tags, datetime.date(2026, 10, 25)) == 'autobuild-2026-09-30-13-08'
    assert latest_month_end(tags, datetime.date(2026, 11, 1)) == 'autobuild-2026-10-24-13-02'
    assert latest_month_end(tags[:1], datetime.date(2026, 11, 1)) is None
    builds = lgpl_static_builds([
        'ffmpeg-n9.0.2-35-g9a9b70bb8c-win64-lgpl-9.0.zip', 'ffmpeg-n9.0.2-35-g9a9b70bb8c-win64-lgpl-shared-9.0.zip',
        'ffmpeg-n9.0.2-35-g9a9b70bb8c-win64-gpl-9.0.zip', 'ffmpeg-n9.0-latest-win64-lgpl-9.0.zip',
        'ffmpeg-N-128013-g6b05263b44-win64-lgpl.zip', 'ffmpeg-n10.0-win64-lgpl-10.0.zip',
        'ffmpeg-n9.0.2-35-g9a9b70bb8c-linux64-lgpl-9.0.tar.xz'])
    assert builds == {(9, 0): ('ffmpeg-n9.0.2-35-g9a9b70bb8c-win64-lgpl-9.0.zip', 'n9.0.2-35-g9a9b70bb8c'),
        (10, 0): ('ffmpeg-n10.0-win64-lgpl-10.0.zip', 'n10.0')}, builds
    assert ffmpeg_ref('n9.0.2-17-g2a571b6068') == '2a571b6068' and ffmpeg_ref('n9.0.3') == 'n9.0.3'
    assert verify_download(1, 'a' * 64, 1, {'x': 'a' * 64, 'y': None}) is None
    assert verify_download(1, 'a' * 64, 1, {'x': None}) and verify_download(1, 'a' * 64, 2, {'x': 'a' * 64})
    assert verify_download(1, 'a' * 64, 1, {'x': 'a' * 64, 'y': 'b' * 64})
    assert replace_tokens('x55329336 55329336 a55329336', [(55329336, 1)]) == 'x55329336 1 a55329336'

    # 両方を書き換え、相手のブロック・行に触れないことを確かめる。新しい値は承認値と重ならないものにする。
    new_sha, new_exe_sha = 'a' * 64, 'b' * 64
    cf_ver, cf_date = '9999.1.0', '2099-01-02'
    old_date = CF_DATE_RE.search(cf['comment'])
    assert old_date, cf['comment']
    cf_upd = {'id': 'kApprovedCloudflared',
        'pairs': [(cf['version'], cf_ver), (cf['download_size'], 123), (cf['download_sha256'], new_sha), (old_date[1], cf_date)],
        'expected': {'version': cf_ver, 'download_size': 123, 'exe_size': 123, 'exe_sha256': new_sha,
            'url': cf['url'].replace(cf['version'], cf_ver)}}
    h1, d1 = apply_update(hpp_text, deps_text, cf_upd)
    assert f'{cf_date} 公開' in h1 and f'{cf_ver}（{cf_date} 公開）' in d1 and '（123 bytes）' in d1
    ver, tag = f'n{ff_series}.99-2-gabcdef1234', 'autobuild-2099-01-31-13-00'
    ff_upd = {'id': 'kApprovedFfmpeg',
        'pairs': [(ff['version'], ver), (ff_tag, tag),
            (ff['download_size'], 1234567), (ff['download_sha256'], new_sha), (ff['exe_size'], 7654321), (ff['exe_sha256'], new_exe_sha)],
        'expected': {'version': ver, 'download_size': 1234567, 'exe_size': 7654321, 'exe_sha256': new_exe_sha,
            'url': f'https://github.com/{FF_REPO}/releases/download/{tag}/ffmpeg-{ver}-win64-lgpl-{ff_series}.zip',
            'exe_in_archive': f'ffmpeg-{ver}-win64-lgpl-{ff_series}\\bin\\ffmpeg.exe'}}
    h2, d2 = apply_update(h1, d1, ff_upd)
    after = parse_approved(h2)
    assert after['kApprovedCloudflared']['version'] == cf_ver and after['kApprovedFfmpeg']['version'] == ver
    assert '1,234,567 bytes' in d2 and '7,654,321 bytes' in d2 and ff['version'] not in d2 and cf_ver in d2
    assert h2.count('\n') == hpp_text.count('\n') and d2.count('\n') == deps_text.count('\n')
    try:
        apply_update(hpp_text, deps_text, dict(ff_upd, expected={'version': 'n0'}))
        raise AssertionError('mismatched expectation must fail')
    except ValueError:
        pass
    print('binary_watch selftest: OK')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--hpp', default=HPP)
    p.add_argument('--deps', default=DEPS)
    sub = p.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('check')
    c.add_argument('--download-dir', required=True)
    c.add_argument('--out', required=True)
    a = sub.add_parser('apply')
    a.add_argument('--result', required=True)
    a.add_argument('--name', required=True)
    sub.add_parser('selftest')
    args = p.parse_args()
    return {'check': cmd_check, 'apply': cmd_apply, 'selftest': cmd_selftest}[args.cmd](args)


if __name__ == '__main__':
    sys.exit(main())
