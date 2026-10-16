# sdrplusplus-j-alert-plugin

*English: [README.md](README.md)*

**J-ALERT**（全国瞬時警報システム）衛星ダウンリンクを復調・復号する
[SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) 用のアウトオブツリー
デコーダプラグインです。RF から直接、地上系と同一の NowcastPacket を
（完全オフライン・地上系参照なしで）復元します。gzip 圧縮された JMA 電文
チャンクは、さらにアラート XML へ展開します。

J-ALERT 衛星リンクは暗号化されていない、標準的な COTS 衛星モデム波形です。

| 項目 | 値 |
|---|---|
| 変調 | BPSK、連続 SCPC 搬送波 |
| シンボルレート | 256 ksym/s（FEC 後 128 kbps）|
| FEC | 畳み込み K=7、生成多項式 (171, 133)₈（"Voyager"）、レート 1/2 |
| スクランブラ | IESS-308 自己同期、1 + x³ + x²⁰ |
| フレーミング | HDLC（0x7E フラグ、ビットスタッフィング）、CRC-16/X.25 FCS |
| ペイロード | NowcastPacket（チャンク化）。JMA 電文チャンクは gzip メンバを内包 → アラート XML |
| 暗号 | なし |

## 信号処理チェーン

```
VFO IQ (1.024 MHz)
 └─ 粗キャリア再生  (2乗信号のブロック FFT ピーク → 2·fc)
 └─ ポリフェーズ リサンプル to 1.024 MHz (4 sps × 256 ksym/s)
 └─ AGC
 └─ 整合フィルタ + シンボルタイミング  (ポリフェーズ RRC β=0.35 + Müller-Müller TED)
 └─ 判定指向 Costas ループ  → 軟値シンボル
 └─ 軟判定 Viterbi  (K=7, (171,133), ストリーミング トレースバック)
 └─ 2回差動復号 + 反転        (BPSK 180° 位相曖昧性を解消)
 └─ IESS-308 自己同期デスクランブル  (s ⊕ s≪3 ⊕ s≪20)
 └─ HDLC デフレーム + デスタッフ  (0x7E フラグ、CRC-16/X.25 検証)
 └─ NowcastPacket 解析  (ヘッダ + チャンクエントリ。チャンク毎に body CRC-32)
 └─ チャンク再結合  → 復元ファイル（地上系とバイト一致）
                       (複数チャンク／複数パケット、id + seq による)
 └─ [任意] gzip 済み JMA 電文チャンク (flags==3, wrmx/eprx/issx/ioeq):
        JMA Socket Packet ヘッダ → gunzip → JMA アラート XML
```

バイト一致で復元される成果物は NowcastPacket／再結合チャンクファイルです。
XML への展開は、gzip 済み JMA 電文チャンク（`flags==3`）にのみ適用される
任意の最終ステップです。非 gzip／非 JMA チャンクも復元・報告されますが、
XML 化はされません。

NowcastPacket／チャンク解析は完全に構造化されています： `NWCS` ヘッダ、
`chunk-entry` テーブル、チャンク毎の `body_crc32` 検証、チャンクメタデータ
（`filename`、`flags==3` = gzip）、コンテンツ分類としての chunk-type コード
（`wrmx`/`eprx`/`issx`/`ioeq`）。チャンクやパケットに分割されたファイルは
`(id, seq, num_chunks)` で再結合されます。パケットの `header_crc32` は意図的に
無視します（変換サーバの既知のバグで常に誤った値になるため）。

レート 1/2 符号のペア位相だけが残る曖昧性です。受信機は 2 本の Viterbi＋フレーム
チェーンを並行実行し（位相ごとに 1 本）、正しい方を HDLC FCS に選ばせます
（誤位相は CRC 不一致のゴミしか生みません）。搬送波位相／ビット極性の曖昧性は
差動復号が吸収するため、反転探索は不要です。

## リポジトリ構成

```
.
├── .devcontainer/        # VS Code Dev Container (Ubuntu 24.04 + クロスツールチェーン + zlib)
├── cmake/toolchains/     # aarch64 / armhf CMake ツールチェーンファイル
├── external/SDRPlusPlus/ # SDR++ ソース (scripts/fetch-sdrpp.sh)
├── scripts/              # fetch-sdrpp.sh, build-all.sh, build-windows.ps1, fetch-windows-deps.py
├── src/
│   ├── main.cpp          # SDR++ モジュール + パネル
│   ├── dsp/              # complex, agc, fft, resampler, RRC, PFB clock sync, BPSK demod
│   ├── fec/              # 畳み込み符号 + ストリーミング軟判定 Viterbi
│   ├── frame/            # 差動, デスクランブラ, HDLC, NowcastPacket
│   ├── decode/           # gunzip, アラートモデル, ビットパイプライン, receiver
│   ├── sink/             # file / TCP JSONL シンク + アラート JSON シリアライザ
│   └── ui/               # sparkline, コンスタレーション表示
├── CMakeLists.txt
└── README.md
```

## ビルド

### Dev Container（全ターゲット）

```sh
scripts/build-all.sh                 # linux-x86_64, linux-aarch64, linux-armhf → dist/
TARGETS="linux-aarch64" scripts/build-all.sh
BUILD_TYPE=Debug scripts/build-all.sh
```

`build-all.sh` は SDR++ ソースを更新してから、各ターゲットを CMake
（Ninja があれば Ninja、なければ Make）で構成・ビルドし、共有ライブラリを
`dist/<target>/` に収集します。

### 単一ネイティブビルド

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

プラグインに必要なのは SDR++ ヘッダと zlib のみです。`sdrpp_core` の
シンボルはロード時に解決されます。

## リリース

GitHub Actions（[.github/workflows/build.yml](.github/workflows/build.yml)）が
Linux の **x86_64** / **x86 (i686)** / **ARM64 (aarch64)** / **ARM32 (armhf)**
と **Windows x64** 向けに push・プルリクエストごとにプラグインをビルドし、
`v*` タグを push するたびに 4 つの `.so` と `.dll` を GitHub Release に添付
します（例: `git tag v1.0.0 && git push --tags`）。zlib は静的リンクされる
ため、各バイナリは自己完結しています。

Windows ジョブは `windows-2022` 上で `scripts/build-windows.ps1` を実行し、
実行時点の SDR++ nightly ビルドに対してリンクします。

### Windows（x64、MSVC）

Windows の DLL は `sdrpp_core` のシンボルを未解決のままにできないため、公式
SDR++ Windows ビルド（取得したヘッダと同じ `master` コミットを追う nightly）
からインポートライブラリを生成してリンクします。必要なもの: Visual Studio 2022
または Build Tools（C++ ワークロード）、git、Python 3.14 以上。

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1
# オプション: -BuildType Debug, -SkipFetch（external/ の依存を再利用）
```

スクリプトは MSVC 環境の読み込み、`scripts/fetch-windows-deps.py` の実行
（SDR++ Windows zip → `.def` エクスポート一覧、conda-forge から fftw3/volk
ヘッダ）、zlib の静的ビルドを行い、`dist\windows-x64\j_alert_decoder.dll`
を出力します。`sdrpp.exe` と同じ場所の `modules` フォルダへコピーしてください。
DLL は読み込む SDR++ ビルドと一致している必要があるため、SDR++ 更新後は
再ビルドしてください。

## SDR++ へのインストール

`j_alert_decoder.so` を SDR++ のプラグインディレクトリ（Linux 既定
`/usr/lib/sdrpp/plugins/`、Windows では `j_alert_decoder.dll` を `sdrpp.exe`
と同じ場所の `modules` フォルダ）にコピーするか、Module Manager から読み込んで
インスタンスを作成します。VFO を J-ALERT 搬送波に合わせると、パネルに
ロック状態、BPSK コンスタレーション、キャリア／Costas オフセット、推定
ビットエラーレート、HDLC フレーム数、最新の復号アラート（標題／見出し／
時刻）が表示されます。復元したファイルは次のように出力できます：

- JSONL を **ファイル**および／または **TCP** ポートへストリーム配信。
  各復元ファイルにつきペイロードをインラインで含みます（gzip 済み JMA 電文は
  アラート XML、非 XML 電文は生バイトの base64）。
  [docs/JSONL_FORMAT.ja.md](docs/JSONL_FORMAT.ja.md)
  （[English](docs/JSONL_FORMAT.md)）参照。
- **Output folder** へ書き出し（**File output** チェックボックスで有効化）：
  復号 JMA 電文は `<timestamp>.xml`、その他（非 XML）電文は生バイトを
  `<timestamp>.<chunk_type>.bin` に保存。

出力パスの既定値は SDR++ 設定ルート配下です： **Output folder** が
`~/.config/sdrpp/j_alert/`、**JSONL file** が
`~/.config/sdrpp/j_alert/decoded.jsonl`。どちらもパネルで変更できます。

すべての出力設定は SDR++ 設定にインスタンスごとに永続化されます。

## 対応プラットフォーム

| ターゲット      | 対応 |
|-----------------|------|
| Linux x86_64    | あり |
| Linux aarch64   | あり |
| Linux armhf     | あり |
| Windows x64     | あり |
| macOS           | なし |

## ライセンス

MIT ライセンス — [LICENSE](LICENSE) を参照。Copyright (c) 2026 KIRISHIKI Yudai。
