"""Builds the documentation site: writes site/*.html, and copies in what is kept elsewhere
(the menu design mocks and the frame-time reports under docs/r3d/reports/).

    python3 tools/site/build.py

The Pages workflow runs this before publishing, so a report committed to
docs/r3d/reports/ shows up on the Performance page at the next deploy.
"""
import glob
import html
import os
import re
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "site")
REPO = "https://github.com/mathiasdbo/Xash3D-FWGS-XBOX"
NAV = [("index.html", "Home"), ("install.html", "Install"), ("settings.html", "Console settings"),
       ("menus.html", "Menus"), ("progress.html", "Progress"), ("performance.html", "Performance"),
       ("recorder.html", "Frame-time recorder"), ("build.html", "Building"),
       ("differences.html", "How it differs"), ("about.html", "About")]


SIGNING = """<h2 id="code-signing-policy">Code signing policy</h2>
<p>Free code signing provided by <a href="https://about.signpath.io/">SignPath.io</a>, certificate by
<a href="https://signpath.org/">SignPath Foundation</a>.</p>
<p><strong>Status:</strong> the Windows packager is not signed yet. The application to SignPath Foundation is in progress,
and until it is approved Windows will warn about an unknown publisher: check the file against
<code>SHA256SUMS.txt</code> in the release.</p>
<ul>
<li><strong>What is signed:</strong> the Windows packager exe of each release (the packager and the asset baker). It is built by
<a href="https://github.com/mathiasdbo/Xash3D-FWGS-XBOX/blob/main/.github/workflows/packager-windows.yml">a GitHub Actions workflow</a> from this repository's
source, and nothing else is signed. The Xbox build (<code>default.xbe</code>) is data beside the exe, not a Windows program.</li>
<li><strong>Roles:</strong> committers and reviewers: <a href="https://github.com/mathiasdbo">mathiasdbo</a>, the project owner,
who also approves every signing request. Members use multi-factor authentication on GitHub and SignPath.</li>
<li><strong>Privacy policy:</strong> this program will not transfer any information to other networked systems unless
specifically requested by the user or the person installing or operating it. The packager connects only to the Xbox whose
address you give it, or finds one when you press <em>Find my Xbox</em>, which scans your own network.</li>
</ul>"""

MILESTONES = """<ol class="milestones">
<li class="now"><strong>The whole campaign playable at 720p</strong> <span class="tag">We are here</span><br>
Today the first campaign maps run on a console, and a 45-minute Office Complex route has been recorded at 720p.
The milestone is Half-Life from start to finish at 720p, which is what the <a href="progress.html">60 fps work</a>
is for.</li>
<li><strong>Multiplayer.</strong> The Xbox build is compiled without networking today (no sockets), so this starts
with the network layer, then the multiplayer screens already drawn in the <a href="menus.html#mocks">design mocks</a>
(system link, internet games, host game, direct connect, player profile) and the on-screen keyboard they share.</li>
<li><strong>Split screen.</strong> Two players on one console. The menu design has the screen, but the engine has no
second local client at all: it would need a second client state, a second view, input from a second controller port and a
listen server hosting both, on a 733 MHz CPU. The project plan keeps it out of the multiplayer milestone until a second
viewport has been measured.</li>
<li><strong>Opposing Force.</strong> Its game libraries have a fork and the packager lists it, but it is not packaged
yet.</li>
<li><strong>Blue Shift.</strong> The same: a game-library fork exists and the packager lists it, but it is not
packaged yet.</li>
</ol>"""

def page(fname, title, body, desc):
    body = body.replace("@@MILESTONES@@", MILESTONES).replace("@@SIGNING@@", SIGNING)
    nav = "".join('<a href="%s"%s>%s</a>' % (h, ' aria-current="page"' if h == fname else "", t) for h, t in NAV)
    html = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%s</title>
<meta name="description" content="%s">
<link rel="stylesheet" href="style.css">
</head>
<body>
<header class="site"><div class="bar">
<a class="name" href="index.html">Xash3D-<b>FWGS</b>-XBOX</a>
<nav>%s</nav>
</div></header>
<main>
%s
</main>
<footer><div>Xash3D-FWGS-XBOX, formerly Resonance3D. GPL-3.0-or-later. Contains no Valve assets.
<a href="%s">Source on GitHub</a>.</div></footer>
</body>
</html>
""" % (title, desc, nav, body.strip(), REPO)
    with open(os.path.join(OUT, fname), "w", encoding="utf-8", newline="\n") as f:
        f.write(html)


page("index.html", "Xash3D-FWGS-XBOX - Half-Life on the original Xbox", """
<h1>Xash3D-FWGS-XBOX</h1>
<p class="lede">Half-Life on a real original Xbox (2001), through a port of the GoldSrc-compatible
Xash3D FWGS engine built against the open-source nxdk homebrew SDK.</p>

<div class="note"><strong>Status.</strong> Half-Life plays on hardware: the engine, the NV2A renderer,
the menu, saves, hardware audio and the offline asset pipeline work, and the first campaign maps run on a
console. It is work in progress. Frame times are measured, not yet where we want them, and Opposing Force and
Blue Shift are not packaged yet.</div>

<h2 id="milestones">Roadmap</h2>
@@MILESTONES@@

<div class="cards">
<div class="card"><h3><a href="install.html">Put it on your Xbox</a></h3>
<p>The packager turns your own Half-Life files and the Xbox build into a folder, a disc image or an FTP upload.</p></div>
<div class="card"><h3><a href="settings.html">Console settings</a></h3>
<p>720p, anti-aliasing, anisotropic filtering, the FPS counter and more, edited from your PC.</p></div>
<div class="card"><h3><a href="build.html">Build it</a></h3>
<p>The PC reference build, the Xbox toolchain and the asset baker.</p></div>
<div class="card"><h3><a href="menus.html">The menus</a></h3>
<p>Built for a controller on a TV: gamepad navigation, button legend, widescreen and 720p layouts.</p></div>
<div class="card"><h3><a href="progress.html">Progress</a></h3>
<p>The 720p, 60 fps target, measured frame times and the optimisations so far.</p></div>
<div class="card"><h3><a href="recorder.html">Measure it</a></h3>
<p>The frame-time recorder: every frame to a file on the console, and a report.</p></div>
<div class="card"><h3><a href="differences.html">How it differs</a></h3>
<p>What was added to Xash3D FWGS, and what was left alone.</p></div>
<div class="card"><h3><a href="about.html">About</a></h3>
<p>Credits, licence, and how the project was written.</p></div>
</div>

<h2>What makes this port different from a recompile</h2>
<ul>
<li><strong>A 64 MB budget shared with the GPU.</strong> The memory ceiling is enforced offline, by the asset
baker, not discovered at runtime. A map that does not fit breaks the build, not the console.</li>
<li><strong>An offline asset pipeline.</strong> Maps, textures and sounds are compiled ahead of time into
<code>.mpak</code> archives with per-map texture atlases, DXT compression and packed lightmaps, so the engine
never parses a WAD on the console.</li>
<li><strong>The console's own hardware.</strong> A renderer for the NV2A GPU (pushbuffer, register combiners,
vertex programs) and audio on the console's audio chip.</li>
<li><strong>Menus made for a controller.</strong> Gamepad navigation, a button legend on every screen, a pause menu,
and layouts for 4:3, 16:9 and 720p. See <a href="menus.html">the menus</a>.</li>
<li><strong>A target and a way to measure it.</strong> 720p at 60 fps, with a frame-time recorder on the console and
a report on your PC. See <a href="progress.html">progress</a>.</li>
</ul>

<h2>You need your own game</h2>
<p>This project contains no Valve assets. Running Half-Life needs your own legally obtained copy (Steam). The
baker reads your install and produces the archives locally; they are not redistributable.</p>

@@SIGNING@@
""", "Half-Life on the original Xbox, through a port of Xash3D FWGS built with nxdk.")

page("install.html", "Install - Xash3D-FWGS-XBOX", """
<h1>Put Half-Life on your Xbox</h1>
<p class="lede">The Xash3D-FWGS-XBOX Packager combines your own Half-Life files with the Xbox build and writes
a folder, an XISO disc image, or uploads straight to the console over FTP.</p>

<h2>What you need</h2>
<ul>
<li>Half-Life from Steam, installed on the PC you run the packager on (it finds the install by itself on Windows).</li>
<li>An original Xbox with a way to run homebrew and an FTP server (the packager was developed against
PrometheOS).</li>
<li>Windows for the one-file exe. Elsewhere, Python 3 with Tk (on Debian or Ubuntu:
<code>sudo apt install python3-tk</code>).</li>
</ul>

<h2>Run the packager</h2>
<p>Windows: download <code>Xash3D-FWGS-XBOX-Packager-&lt;version&gt;-windows.zip</code> from the
<a href="%(repo)s/releases">releases page</a>, unzip it and run the exe. Keep the <code>payload</code> folder next to it: it
holds the Xbox build. The exe is not signed yet, so Windows says <em>Windows protected your PC</em>: check the zip against
<code>SHA256SUMS.txt</code>, then choose <em>More info</em> and <em>Run anyway</em>
(<a href="#code-signing-policy">code signing policy</a>). Anywhere else: unzip the portable package and run</p>
<pre><code>python3 r3d_packager.py</code></pre>
<ol>
<li>Pick the game. Half-Life is available; Opposing Force and Blue Shift are listed and will follow.</li>
<li>Check the folder it found. The engine needs <code>delta.lst</code> (Half-Life 1.1.0.0 or later) and
<code>resource/gameui_english.txt</code>; the window says what is missing.</li>
<li>Choose <strong>Folder</strong> (copy it to the console yourself), <strong>XISO</strong>, or <strong>Xbox over
FTP</strong>, and where to save or send.</li>
<li>Build. The first export takes about two minutes on a modern machine, because the maps are baked.</li>
</ol>
<p>Without a window:</p>
<pre><code>python3 r3d_packager.py --cli --steam "&lt;Half-Life folder&gt;" --format folder|xiso --out &lt;folder&gt;</code></pre>

<h2 id="see-it">See it work</h2>
<p>A 27-second recording of the packager (no sound): choose the game, pick <em>Send to the Xbox over FTP</em>, find the
console and send. It ends while the first export is under way, which is the slow step: the packager bakes the Xbox data from
your own game files, as the log says.</p>
<figure class="shot">
<video controls preload="metadata" playsinline poster="media/packager-demo-poster.jpg">
<source src="media/packager-demo.mp4" type="video/mp4">
<a href="media/packager-demo.mp4">Download the video</a>
</video>
<figcaption>The packager sending Half-Life to an Xbox over FTP.</figcaption>
</figure>
<div class="shots">
<figure>
<a href="media/packager-package.png"><img src="media/packager-package.png" loading="lazy" width="802" height="892"
alt="The Package tab: the game, the Half-Life folder it found, three export choices (a folder, an XISO disc image, or send to the Xbox over FTP), and a Build the folder button."></a>
<figcaption><strong>Package.</strong> The game, the install it found, and how to export.</figcaption>
</figure>
<figure>
<a href="media/packager-ftp.png"><img src="media/packager-ftp.png" loading="lazy" width="802" height="892"
alt="The Package tab with Send to the Xbox over FTP chosen: the Xbox address, login and games folder fields are active, with Find my Xbox and Look for it buttons."></a>
<figcaption><strong>Send over FTP.</strong> Find the console, pick its games folder, send. The address shown is an example.</figcaption>
</figure>
<figure>
<a href="media/packager-working.png"><img src="media/packager-working.png" loading="lazy" width="976" height="1080"
alt="The packager at 7 percent while working: the progress bar, and a log reading that it found an Xbox and is baking the Xbox data from the game files for 125 maps, the menu art, the skies and the music."></a>
<figcaption><strong>Working.</strong> The first export bakes the Xbox data and keeps it for next time.</figcaption>
</figure>
<figure>
<a href="media/packager-console-settings.png"><img src="media/packager-console-settings.png" loading="lazy" width="802" height="892"
alt="The Console settings tab: an install on the Xbox or a folder on the PC, then the display settings such as output, aspect, anti-aliasing and anisotropic filtering, presets, and the files in userconfig.d."></a>
<figcaption><strong>Console settings.</strong> <a href="settings.html">Change what the console runs with</a>, from your PC.</figcaption>
</figure>
</div>

<h2>What ends up on the console</h2>
<ul>
<li>Your <code>valve</code> folder, plus <code>default.xbe</code> titled "Half-Life", the menu font, colours,
button art and translations.</li>
<li>One <code>.mpak</code> per map, baked from your files. The retail <code>.bsp</code> of a baked map is left
off, because a loose <code>.bsp</code> beside its archive is loaded instead and the world then draws black.</li>
<li>The menu background tiles, the 19 skies as DXT1 <code>.dds</code>, and the music as ADPCM
<code>.xwav</code>.</li>
<li><code>video.cfg</code> with <code>r_refdll "nv2a"</code>, and the menu's control images.</li>
</ul>
<p>The result is cached (<code>%%LOCALAPPDATA%%\\Xash3D-FWGS-XBOX Packager\\bake</code> on Windows) and reused until the
baker or your game files change.</p>

<h2>Uploading over FTP safely</h2>
<p>"Find my Xbox" scans the network for the console and its games folders. Before it changes an existing
install, the packager surveys it and asks. It never replaces your saves or settings, and it moves the old
<code>default.xbe</code> aside as <code>default.xbe.before-packager</code> instead of overwriting it.</p>
<p>Next: <a href="settings.html">change the console's settings</a> from the second tab.</p>

@@SIGNING@@
""" % {"repo": REPO}, "How to put Half-Life on an original Xbox with the packager.")

page("settings.html", "Console settings - Xash3D-FWGS-XBOX", """
<h1>Console settings</h1>
<p class="lede">The packager's second tab reads and changes what an install runs with, on the console over FTP or
in an exported folder on your PC.</p>

<h2>How it works</h2>
<p>The engine runs <code>config.cfg</code> (the in-game menu's) and then every
<code>userconfig.d/*.cfg</code> in name order; later files win. The tab writes one file of its own,
<code>userconfig.d/50-packager.cfg</code>, so it overrides the menu only for what it sets. A setting left on
"(don't set)" is not written and the menu keeps deciding it.</p>
<ul>
<li><strong>Find my Xbox</strong> fills in the address, looks through the games folders for installs of this
game and reads the one it finds.</li>
<li><strong>What the console runs with</strong> shows each setting's value and the file it comes from, and
warns when a later file overrides yours or loads a map on every start.</li>
<li>Each <code>userconfig.d</code> file has a tick: it renames the file to or from <code>.off</code>, which is
how the engine skips it. Nothing is overwritten; saving an edited file keeps the old one as
<code>.before-packager</code>.</li>
<li>Presets: <strong>Sharp</strong> (720p, 8x anisotropy), <strong>Measure</strong> (recorder on, label, memory
report) and <strong>Clear all</strong>.</li>
</ul>

<h2>Settings</h2>
<table>
<tr><th>Setting</th><th>Values</th><th>What it does</th></tr>
<tr><td><code>r3d_vid_mode</code></td><td>0, 1</td><td>Output: 480p or 720p. Read at boot.</td></tr>
<tr><td><code>r3d_vid_aspect</code></td><td></td><td>Aspect of the output.</td></tr>
<tr><td><code>nv2a_r_aa</code></td><td>0, 1, 2, 3</td><td>Anti-aliasing of the 3D view: <code>0</code> off, <code>1</code> 2x MSAA,
<code>2</code> Quincunx (QAA), <code>3</code> 4x MSAA. Saved with the menu's settings and changes live. Details
<a href="#aa">below</a>.</td></tr>
<tr><td><code>nv2a_r_anisotropy</code></td><td>1, 2, 4, 8</td><td>Anisotropic filtering (default 2).</td></tr>
<tr><td><code>nv2a_r_texturenearest</code></td><td>0, 1</td><td>Nearest-neighbour texture filtering.</td></tr>
<tr><td><code>nv2a_r_lodbias</code></td><td></td><td>Texture level-of-detail bias.</td></tr>
<tr><td><code>cl_showfps</code></td><td>0, 1, 2</td><td>FPS counter.</td></tr>
<tr><td><code>host_frametime_analysis</code></td><td>0, 1, 2</td><td>Frame-time recorder: off, record, record plus overlay.
See <a href="recorder.html">the recorder</a>.</td></tr>
<tr><td><code>host_frametime_label</code></td><td>up to 22 characters</td><td>Names a recording session; letters, digits,
<code>-</code> and <code>_</code> only.</td></tr>
<tr><td><code>r3d_memreport</code></td><td></td><td>Memory report written beside the recording.</td></tr>
<tr><td><code>r3d_snd_meter</code></td><td></td><td>On-screen sound meter.</td></tr>
<tr><td><code>net_graph</code></td><td></td><td>Network graph overlay.</td></tr>
</table>
<h2 id="aa">Anti-aliasing (<code>nv2a_r_aa</code>)</h2>
<p>This is the GPU's own multisampling. The 3D view is drawn into a larger surface and scaled down to the screen in
one pass; the HUD, console and menus are drawn afterwards at the screen's own resolution, so they stay sharp. In the
menu the same choice is Options, Display, Anti-aliasing (Off, 2x, Quincunx, 4x).</p>
<table>
<tr><th>Value</th><th>Menu name</th><th>What it is</th><th>Extra memory at 480p</th></tr>
<tr><td><code>0</code></td><td>Off</td><td>The 3D view is drawn straight into the screen. The default.</td><td>none</td></tr>
<tr><td><code>1</code></td><td>2x</td><td>2x MSAA: two samples per pixel, resolved with a bilinear filter between them.</td><td>about 2.3 MiB for each of the two surfaces (colour and depth)</td></tr>
<tr><td><code>2</code></td><td>Quincunx</td><td>QAA: the same two samples as 2x, resolved with the GPU's Quincunx filter, which
blends in neighbouring pixels as well. Softer than 2x, at the same cost.</td><td>the same as 2x</td></tr>
<tr><td><code>3</code></td><td>4x</td><td>4x MSAA: a 2x2 block of samples per pixel, resolved at the block's centre.
The smoothest edges, and the most memory and fill cost.</td><td>about 4.7 MiB for each of the two surfaces</td></tr>
</table>
<ul>
<li>Memory is taken between frames and given back before a map's own buffers are made. At 720p the surfaces are
larger: 4x there needs about 25 MiB more, which is why it was judged out as a default.</li>
<li>If the surfaces cannot be had, the engine keeps drawing without anti-aliasing, and does not ask again until you
change the setting or the map.</li>
<li>Quincunx on the console is not yet confirmed by a hardware check: the emulator draws that filter as a plain
blur, so judge it on a console.</li>
<li>Transparent edges (decals, sprites) alias more than solid edges under MSAA.</li>
</ul>

<p>Audio options (backend, rate, hardware audio) and developer switches (GPU skinning, culling) are in the tab
too; developer settings are behind a tick. Video output, sound backend, rate and hardware audio are read at
boot, so a change shows at the next start of the game.</p>
""", "Edit the Xbox build's video, graphics and diagnostic settings from the packager.")

page("build.html", "Building - Xash3D-FWGS-XBOX", """
<h1>Building</h1>
<p class="lede">The PC reference build is upstream's. The Xbox build uses the nxdk toolchain and the scripts in
<code>tools/</code>.</p>

<h2>PC reference build</h2>
<p>The build system is unchanged from Xash3D FWGS:</p>
<pre><code>./waf configure -T release
./waf build</code></pre>
<p><code>ref/gl</code> stays alive until the Vulkan renderer matches it; it is the only thing that tells us what
correct looks like.</p>

<h2>Xbox build</h2>
<p>The toolchain is in <code>deps/nxdk</code>. nxdk has no dynamic library loading, so everything is linked
statically into the XBE.</p>
<table>
<tr><th>Script</th><th>What it does</th></tr>
<tr><td><code>tools/xbox-sweep.sh engine</code></td><td>Compiles every engine C file for the Xbox and summarises the failures.</td></tr>
<tr><td><code>tools/xbox-game-link-check.sh hl --include-client --include-freevgui --xbe &lt;path&gt;</code></td>
<td>Links the Half-Life <code>default.xbe</code>.</td></tr>
<tr><td><code>tools/xbox-xiso.sh</code></td><td>Packs a disc from it.</td></tr>
</table>
<p>Conventions, gates and the rules for contributions are in <code>AGENTS.md</code> in the repository.</p>

<h2>The asset baker</h2>
<p><code>tools/asset-studio</code> is a separate CMake project that builds for the host, not the console. It
reads a retail install and writes the <code>.mpak</code> archives, the DXT1 skies and the music files. The
packager runs it for you; the Windows exe carries a copy. On Linux, build it first with cmake and g++:</p>
<pre><code>python3 tools/packager/build_baker.py</code></pre>

<h2>The packager</h2>
<pre><code>python tools/packager/build_baker.py                              # builds bin/AssetStudio
python tools/packager/make_payload.py --xbe &lt;clean main build&gt;   # builds payload/
python tools/packager/release.py --xbe &lt;build&gt; --version vX.Y   # builds, does not publish
python -m unittest discover -s tools/packager/tests              # tests</code></pre>
<p><code>release.py</code> needs PyInstaller. The Windows exe is built on Windows; Linux users get the portable zip.
A release carries the stable Xbox build: a clean build of <code>main</code>, and the release notes name its commit.</p>

<h2>Testing in an emulator</h2>
<p>The scripts <code>tools/xemu-*.sh</code> run builds in <a href="https://github.com/xemu-project/xemu">xemu</a>,
the Xbox emulator used for development. An emulator does not replace the console for performance numbers.</p>
""", "How to build the Xbox port, the PC reference build and the asset baker.")

page("recorder.html", "Frame-time recorder - Xash3D-FWGS-XBOX", """
<h1>Frame-time recorder</h1>
<p class="lede">Every frame's time, split into CPU work and waiting, plus what the renderer did in that frame,
written to a file on the console's drive and turned into an HTML report on your PC.</p>

<h2>Turn it on</h2>
<p>Use the <a href="settings.html">Console settings</a> tab (the <strong>Measure</strong> preset), or put one line
in a file in <code>valve/userconfig.d/</code> on the console:</p>
<pre><code>host_frametime_analysis 2</code></pre>
<p><code>0</code> is off (the default), <code>1</code> records only, <code>2</code> records and draws a frame-time
graph and the work counters over the HUD. The setting is not saved to <code>config.cfg</code> on purpose:
delete the line and the next boot records nothing.</p>
<p>Give the session a label that says what you are measuring:</p>
<pre><code>host_frametime_label "office-run"</code></pre>
<p>It goes into the header and the file name, cleaned to letters, digits, <code>-</code> and <code>_</code> and cut to
22 characters.</p>

<h2>What it records</h2>
<ul>
<li>Each frame goes into a ring in memory and is written to the drive every 5 seconds and at each map start. A
power-off loses at most the last 5 seconds.</li>
<li>A session is written as segments: a new file at each map start, and when one reaches 8 MiB (about 3.3
minutes at 60 fps).</li>
<li>The header names the build; its "xbe" field is the XBE's link timestamp in hexadecimal, so a recording can
always be matched to the build that made it.</li>
</ul>

<h2>Read the recordings</h2>
<p><code>tools/frametime-report.py</code> fetches the recordings from the console over FTP, prints a summary or
lists sessions, and writes the HTML report. Load one session at a time: a whole folder of long recordings can
exhaust memory.</p>

<h2>Reading the numbers</h2>
<p>An average frame rate hides the frames that hurt. Look at how many frames take longer than 33 ms (a dropped
frame at 30 fps), the worst frames, and any stall over a quarter of a second. A session of half an hour at
roughly 58 fps average with well under 2%% of frames over 33 ms on most maps is a smooth one.</p>
<p>Published reports are on the <a href="performance.html">performance page</a>, and what the recordings showed and what
was changed because of them is on the <a href="progress.html">progress page</a>.
The full reference is <code>docs/r3d/frametime-recorder.md</code> in the repository.</p>
""".replace("%%", "%"), "How to record and read frame times on the Xbox build.")

page("differences.html", "How it differs - Xash3D-FWGS-XBOX", """
<h1>How it differs from Xash3D FWGS</h1>
<p class="lede">The engine core is still Xash3D FWGS. The port adds a platform layer, a renderer, an asset pipeline and
tools around it.</p>

<h2>Still upstream</h2>
<p>The client and server, networking, physics, the console, save and load, and the GoldSrc compatibility layer.
The version is upstream's (0.21), and other targets keep upstream's name.</p>

<h2>Added or replaced for the Xbox</h2>
<table>
<tr><th>Part</th><th>Where</th><th>What</th></tr>
<tr><td>NV2A renderer</td><td><code>ref/nv2a</code></td><td>Pushbuffer submission, register combiners and vertex programs for the console's GPU.</td></tr>
<tr><td>Platform layer</td><td><code>engine/platform/xbox</code></td><td>Entry point, video modes, input, storage, memory reporting, the frame-time recorder, hardware audio.</td></tr>
<tr><td>Archives</td><td><code>filesystem/mpak*</code></td><td>The <code>.mpak</code> per-map archive and its loader; the console never reads a WAD.</td></tr>
<tr><td>Asset baker</td><td><code>tools/asset-studio</code></td><td>Offline compiler from a retail install to archives, enforcing the memory budget.</td></tr>
<tr><td>Packager</td><td><code>tools/packager</code></td><td>Folder, XISO or FTP output, plus remote console settings.</td></tr>
<tr><td>Menus</td><td><code>3rdparty/mainui</code></td><td>Controller-first screens, pause menu, button legend, widescreen and 720p layouts, baked backgrounds,
fonts and translations. See <a href="menus.html">the menus</a>.</td></tr>
<tr><td>Frame-time recorder</td><td><code>engine/platform/xbox</code>, <code>tools/frametime-report.py</code></td><td>Per-frame CPU, GPU-wait and subsystem timings recorded on the console. See <a href="recorder.html">the recorder</a>.</td></tr>
<tr><td>Game libraries</td><td>forks of the Half-Life SDK</td><td>Carry the Xbox changes; see the repository's related-repositories table.</td></tr>
</table>
<p>The Xbox build reports itself as <code>Xash3D FWGS-XBOX 0.21 (xbox-i386 build N)</code>. The OS reads
<code>xbox</code> because nxdk targets i386-pc-win32 and would otherwise say <code>win32</code>.</p>

<h2>The divergence ledger</h2>
<p>Every deliberate deviation from upstream is recorded the same day it is written, with what changed, why, in
what form (a new file, an <code>#ifdef</code>, a replacement), the merge risk, and what it would take to remove
it. The point is that merging upstream stays possible. The ledger is
<code>docs/r3d/divergences.md</code>; the plan behind the work is <code>docs/r3d/fork-plan.md</code>.</p>
<p>Preferred forms, cheapest to most expensive to maintain: a new file in our own directory; a new file inside an
upstream directory; <code>#ifdef XASH_XBOX</code> around upstream code; rewriting an upstream function (needs
explicit justification). Changing behaviour with no <code>#ifdef</code> is forbidden unless it is a genuine
upstream bug, in which case the patch goes upstream first.</p>
""", "What the Xbox port adds to Xash3D FWGS and what it leaves alone.")

page("about.html", "About - Xash3D-FWGS-XBOX", """
<h1>About</h1>

<h2>Name</h2>
<p>The project was called Resonance3D and is now Xash3D-FWGS-XBOX. Older changelog entries, the divergence
ledger and the <code>// Resonance3D (#NN)</code> markers in the source keep the old name.</p>

<h2>Credits</h2>
<p>This project is a derivative work. It exists because of:</p>
<ul>
<li><a href="https://github.com/FWGS/xash3d-fwgs">Xash3D-FWGS</a> and its contributors, the engine this port is built on.</li>
<li>Uncle Mike, original author of Xash3D, whose copyright headers are throughout the tree.</li>
<li>Valve, for Half-Life and the original GoldSrc engine and SDK.</li>
<li><a href="https://github.com/XboxDev/nxdk">XboxDev / nxdk</a>, the open-source homebrew SDK this port is built
against, including pbkit, the NV2A pushbuffer library.</li>
<li><a href="https://github.com/FWGS/library-suffix">FWGS/library-suffix</a>, platform detection for the build system.</li>
<li><a href="https://github.com/xemu-project/xemu">xemu</a>, the Xbox emulator used for development, and
<a href="https://github.com/Mayo1970/xash3d-fwgs">xashPS3</a>, a sibling console port whose porting log was a
useful map.</li>
</ul>

<h2>Licence</h2>
<p>Xash3D-FWGS is licensed GPL-3.0-or-later, and so is this port. This repository contains no Valve assets.
Running the game requires your own legally obtained copy of Half-Life; the baker's archives are made locally and
are not redistributable.</p>

<h2>AI disclosure</h2>
<p>Significant portions of this port, including platform bring-up, build-system integration, the renderer, audio and
input backends, the asset pipeline and bug fixes across the engine, were developed with the assistance of Claude
(Anthropic), used as a coding tool under human direction and review.</p>

<h2>Related repositories</h2>
<table>
<tr><th>Repository</th><th>What it holds</th></tr>
<tr><td><a href="https://github.com/mathiasdbo/halflife-updated-xash3d">halflife-updated-xash3d</a></td><td>Half-Life game libraries, with the Xbox changes</td></tr>
<tr><td><a href="https://github.com/mathiasdbo/halflife-op4-updated">halflife-op4-updated</a></td><td>Opposing Force game libraries</td></tr>
<tr><td><a href="https://github.com/mathiasdbo/halflife-bs-updated">halflife-bs-updated</a></td><td>Blue Shift game libraries</td></tr>
<tr><td><a href="https://github.com/mathiasdbo/mainui_cpp">mainui_cpp</a></td><td>The menu</td></tr>
<tr><td><a href="https://github.com/mathiasdbo/nxdk-ex">nxdk-ex</a></td><td>Audio (APU) and other Xbox extensions on top of nxdk</td></tr>
</table>
""", "Credits, licence and AI disclosure for Xash3D-FWGS-XBOX.")
page("menus.html", "Menus - Xash3D-FWGS-XBOX", """
<h1>The menus</h1>
<p class="lede">The menu is built for a controller on a television: no mouse, a legend of button glyphs on every
screen, and layouts that follow the console's 4:3, 16:9 and 720p outputs.</p>

<h2>What it is made of</h2>
<ul>
<li><strong>The main menu</strong> is the <code>mainui_cpp</code> menu (our fork, in <code>3rdparty/mainui</code>).
It already moves a single cursor with a gamepad; the fork adds the Xbox screens, the legend bar and the fonts.</li>
<li><strong>The console and the HUD</strong> are drawn by the engine itself. All menu layers draw through the same
2D primitives of the NV2A renderer, so the renderer's 2D pass is what makes any menu appear.</li>
<li><strong>FreeVGUI</strong>, a reimplementation of Half-Life's GUI library, is a separate layer. Single-player
Half-Life does not need it. It is planned for multiplayer panels (scoreboard, message of the day, team menus).</li>
</ul>

<h2>Screens</h2>
<table>
<tr><th>Screen</th><th>What is on it</th></tr>
<tr><td>Main</td><td>Resume (in game), New Game, Training Room, Load Game, Save Game (in game), Options. Console,
multiplayer, custom game and quit are hidden on the console.</td></tr>
<tr><td>Pause</td><td>Opened with Start in single-player. The game freezes and dims behind it, and the chapter and
difficulty are shown. Resume, Save, Load, Options, Quit.</td></tr>
<tr><td>Options</td><td>Controller, Display, Audio, Game.</td></tr>
<tr><td>Controller</td><td>Standard, Southpaw or Legacy layout, and button customisation. The pause button cannot be reassigned.</td></tr>
<tr><td>Display</td><td>Brightness, texture filtering, 720p, aspect (dashboard, 4:3, 16:9), anti-aliasing and anisotropic
filtering, and the current resolution and refresh rate.</td></tr>
<tr><td>Audio</td><td>Sound, music and HEV-suit sliders, <em>Hardware audio</em> and <em>Voices volume</em>. Hardware
audio takes effect at the next start.</td></tr>
<tr><td>Game</td><td>Auto-aim, crosshair, fast weapon switch.</td></tr>
<tr><td>Load and Save</td><td>With a screenshot preview of each save.</td></tr>
</table>

<h2 id="mocks">The design mocks</h2>
<p>The screens were designed first, as interactive HTML boards at 1280x960 (4:3), and the menu was built to match.
Pick a screen below. The board has 21 screens, and not all of them exist in the engine yet: Main, Pause, Options,
Controller, Display, Audio, Game, Load and the new-game difficulty are built; Multiplayer, System Link, Host Game,
Direct Connect, Split Screen, Player Profile, Internet Games, Master Servers and the on-screen keyboard are designs
for later work. The mocks use the amber palette, and the in-game menu draws the same layout with the engine's own
fonts.</p>
<div class="mockbar">
<label for="mockscreen">Screen</label>
<select id="mockscreen" disabled>
<option>Main menu</option><option>New Game - difficulty</option><option>Load Game</option><option>Settings</option>
<option>Options</option><option>Controller - scheme select</option><option>Customize Controls</option>
<option>Audio</option><option>Display</option><option>Game</option><option>Multiplayer</option>
<option>System Link - server browser</option><option>Host Game</option><option>Direct Connect - IP entry</option>
<option>Split Screen - 2 players</option><option>Player Profile</option><option>Internet Games - master server browser</option>
<option>Master Servers - add / edit</option><option>On-screen keyboard (shared)</option><option>Pause - single player</option>
<option>Game menu - multiplayer</option>
</select>
<button type="button" id="mockload">Load the mocks (9 MB)</button>
</div>
<div class="mockframe" id="mockframe"><iframe id="mockiframe" scrolling="no" title="Half-Life Xbox menu design mocks" loading="lazy"></iframe></div>
<p class="mocklinks"><a href="mocks/half-life-xbox-menu.html">Open the whole board in its own page</a> &middot;
<a href="%(repo)s/blob/main/docs/r3d/design/xbox%%20menu/Half-Life%%20Xbox%%20Menu.html">The source file in the repository</a></p>
<script>
(function(){
var sel=document.getElementById('mockscreen'),btn=document.getElementById('mockload'),
    box=document.getElementById('mockframe'),fr=document.getElementById('mockiframe');
function fit(){var s=box.clientWidth/1320;fr.style.transform='scale('+s+')';box.style.height=(990*s)+'px';}
function show(){
  try{
    var d=fr.contentDocument,b=d.querySelectorAll('.board');
    var y=b.length>sel.selectedIndex?b[sel.selectedIndex].offsetTop:48+sel.selectedIndex*1038;
    fr.contentWindow.scrollTo(0,y);
  }catch(e){}
}
btn.addEventListener('click',function(){
  btn.disabled=true;btn.textContent='Loading...';
  fr.addEventListener('load',function(){sel.disabled=false;btn.style.display='none';fit();setTimeout(show,1500);setTimeout(show,4000);});
  fr.src='mocks/half-life-xbox-menu.html';box.classList.add('on');fit();
});
sel.addEventListener('change',show);
window.addEventListener('resize',fit);
})();
</script>

<h2>Why this is more than a port of the menu</h2>
<ul>
<li><strong>Navigation without a mouse.</strong> The cursor skips items that are greyed out or mouse-only, and the
order the controls are registered in is the order the pad visits them, so each screen is checked against that.
Two screens had it wrong and were fixed. A button legend (A, B, X, Y glyphs and what they do) is drawn at the
bottom of every screen.</li>
<li><strong>Widescreen and 720p.</strong> When the output is 16:9 the engine reports 854x480, so the field of view,
the HUD and the menu all widen instead of stretching. The menu scales to the screen's height and the HUD anchors to
its edges. <code>r3d_vid_aspect</code> forces 4:3 or 16:9 and <code>r3d_vid_mode 1</code> selects 1280x720, which needs
an HDTV pack with 720p allowed in the dashboard.</li>
<li><strong>Memory.</strong> The menu releases its fonts and background while you play and loads them back when
the menu opens. The backgrounds are baked offline into tiles, with a compressed variant that cuts the menu's textures
to about a third.</li>
<li><strong>Fonts and text.</strong> Fira Sans Condensed, because the design's first choice lacked Cyrillic and some
Central European letters. Strings are looked up by their English text, and a German translation ships as a proof.</li>
<li><strong>Look.</strong> An amber palette from the menu design, shipped as a data file; a focused button glows.</li>
<li><strong>Testing without a console.</strong> Scripts drive the menus in the emulator with a virtual controller
(<code>tools/xemu-pad.py</code> and friends).</li>
<li><strong>Leaving the game.</strong> Start, Back and both triggers together return to the dashboard.</li>
</ul>

<h2>Still to do</h2>
<ul>
<li>An on-screen keyboard (the hook exists; the window is not written).</li>
<li>The FreeVGUI backend and gamepad navigation by widget position, for multiplayer panels.</li>
<li>Multiplayer screens, and VGUI2 for games that need it (not before Half-Life runs well on hardware).</li>
<li>A speaker-output row (stereo, mono, surround) once the console's setting layout is understood.</li>
<li>A font with a licence that covers the small help text.</li>
</ul>
<p>The design notes are <code>docs/r3d/ui-architecture.md</code>, and each change is in
<code>CHANGELOG.md</code> and the divergence ledger.</p>
""" % {"repo": REPO}, "The Xbox build's menus: controller navigation, screens, design mocks, widescreen and 720p, and what is still to do.")

page("progress.html", "Progress - Xash3D-FWGS-XBOX", """
<h1>Progress</h1>
<p class="lede">Where the performance work stands: the target, what has been measured and on what, what has landed,
and what is still open.</p>

<h2 id="milestones">Next milestones</h2>
@@MILESTONES@@

<h2>The target: 720p at 60 fps</h2>
<p>The goal is Half-Life at 1280x720 and 60 frames per second, which gives each frame 16.7 ms for the CPU and the GPU
together. The recorder draws the 16.7 ms and 33.3 ms lines and reports the share of frames over each.</p>
<ul>
<li>720p is a real output mode today: <code>r3d_vid_mode 1</code>, or the Display menu, with anisotropic filtering and
anti-aliasing. 4x anti-aliasing at 720p was judged out for memory (about 25 MiB more).</li>
<li>No per-map frame-rate goals are written down yet. The performance review asks for exit criteria with a number for
each phase, which is the next thing to settle.</li>
</ul>

<div class="note"><strong>Read the numbers with care.</strong> Console figures are from a real Xbox. Figures marked
<em>emulator</em> come from xemu on a PC: good for comparing one change against another, not a Pentium III timing.
The performance review itself says most per-change numbers have not been repeated on the console.</div>

<h2>Where it stands on the console</h2>
<ul>
<li><strong>Original baseline:</strong> the owner's route averaged 44 fps on a real console, with four maps near 30.
The same route in the emulator held 60 on almost every map, because the emulator is not the console.</li>
<li><strong>720p, 45-minute Office Complex route:</strong> frames over 33 ms went from about 5% to 1-3% on c1a2, and from
4.5% to 1% on c1a2a.</li>
<li><strong>Still open:</strong> the Office Complex still has 1-3% of frames over 33 ms. The slowest maps found are
c1a1b (water), c1a0c and c1a0e (beams). Level-load stalls remain, and many changes below still say "console numbers to
be recorded".</li>
</ul>

<h2>How frames are measured</h2>
<p>The <a href="recorder.html">frame-time recorder</a> has grown with the work. It began as a ring of frame times
written to the drive, an overlay and a report. Since then it gained 20 per-frame renderer counters, a flag for load
frames and a stalls table, then CPU spans per subsystem (scene, particles, beams, mixer, network, server game, player
movement, client entities, saves, menu, music, load stages), labelled segments per map, and report commands to
compare two sessions or fetch them from the console or the emulator. The recorder cannot time the GPU itself (the
SDK has no GPU timestamp), so it records how long the CPU waited for it.</p>
<p>Stalls found this way: one 350-1000 ms spike per map turned out to be mostly level changes, and six longer ones were
autosaves.</p>

<h2>Optimisations that landed</h2>
<p>Numbers are the change's own measurement; <em>emu</em> marks the emulator.</p>
<table>
<tr><th>Area</th><th>Change</th><th>Measured</th></tr>
<tr><td>Libraries</td><td>Fast <code>memcpy</code>, <code>memset</code>, <code>strlen</code> and friends; nxdk libraries rebuilt at -O2</td>
<td>c1a0e CPU 12.3 to 7.6 ms; then 7.55 to 6.83 ms (emu)</td></tr>
<tr><td>Models</td><td>Characters skinned and lit on the GPU; vertex programs kept resident; chrome meshes on the GPU</td>
<td>Studio CPU 2.64 to 0.50 ms in a firefight; chrome 0.95 to 0.32 ms (emu)</td></tr>
<tr><td>Models</td><td>Cheaper bone setup in the game library; each model's light result cached</td>
<td>Bone setup 5.35 to 4.09 us per bone (console); cached light callbacks 0.47-0.57 ms per frame at 720p</td></tr>
<tr><td>World</td><td>Face walk limited to the player's area; adjacent faces of a brush entity merged; water warp on the GPU</td>
<td>c1a1b world walk 1.52 to 0.21 ms; water 1.31 to 0.04 ms (emu)</td></tr>
<tr><td>Frame pacing</td><td>The CPU no longer waits for the GPU at the end of a frame</td>
<td>A frame costs about the larger of the two</td></tr>
<tr><td>Effects</td><td>Cheaper beams, particles and decal lookups</td>
<td>Beams c1a0c 2.16 to 1.41 ms; static decals 95 to 53 ms (emu)</td></tr>
<tr><td>Audio</td><td>Integer mixer stepping, hardware audio, baked ADPCM music with read-ahead, speech streaming, 48 kHz mix</td>
<td>Hardware audio about 0.30 ms per frame, down from 0.45 (console)</td></tr>
<tr><td>Network and server</td><td>Unchanged entities not re-sent; bounds tested first in clipping; finer entity grid; smaller snapshots</td>
<td>Server send 0.60 to 0.28 ms (emu); 5-6 ms per frame saved in the offices</td></tr>
<tr><td>Loading</td><td>Entity lump parsed once, directory cache, fewer file calls, a dropped-frame fix</td>
<td>Route loads 10.7 to 8.7 s (emu); the bug cost 70-900 ms per level change (console)</td></tr>
<tr><td>Menu</td><td>Glyph atlas fast path; fonts and backgrounds released during play</td>
<td>Font rebuild 84-88 to 23-26 ms (emu)</td></tr>
</table>

<h2>Next</h2>
<ul>
<li>Repeat the emulator-only results on the console and write down the numbers.</li>
<li>Skinning limit: about 50 bones fit in the GPU's vertex-program registers, and the game allows up to 128.</li>
<li>The baker's next phase: draw lists per cluster and material, bone partitioning, 16-bit lightmaps, ordering that
suits the GPU's vertex cache.</li>
<li>Lightstyles: the baker bakes style 0 only, so switching and flickering lights are not baked yet.</li>
<li>Threaded save and load I/O. SSE only once a profile on hardware says it pays.</li>
<li>Per-map frame-rate goals, then the 60 fps work on the maps that miss them.</li>
</ul>
<p>The detail is in <code>docs/r3d/xbox-performance-review.md</code>, <code>CHANGELOG.md</code> and
<code>docs/r3d/frametime-recorder.md</code>.</p>
""", "The 720p and 60 fps target, measured frame times, the optimisations that landed and what is next.")


# ---- Performance: one entry per report in docs/r3d/reports/*.html ------------------------------

REPORTS_SRC = os.path.join(ROOT, "docs", "r3d", "reports")
MOCK_SRC = os.path.join(ROOT, "docs", "r3d", "design", "xbox menu", "Half-Life Xbox Menu.html")


def _text(fragment):
    return html.unescape(re.sub(r"\s+", " ", re.sub(r"<[^>]+>", "", fragment or ""))).strip()


def _first(pattern, text):
    m = re.search(pattern, text, re.S)
    return _text(m.group(1)) if m else ""


def read_reports():
    reports = []
    for path in sorted(glob.glob(os.path.join(REPORTS_SRC, "*.html"))):
        name = os.path.basename(path)
        with open(path, encoding="utf-8", errors="replace") as f:
            t = f.read()
        title = _first(r"<title>(.*?)</title>", t) or name
        eyebrow = _first(r'class="eyebrow"[^>]*>(.*?)</div>', t)
        lede = _first(r'class="lede"[^>]*>(.*?)</p>', t)
        if len(lede) > 320:
            lede = lede[:317].rsplit(" ", 1)[0] + "..."
        low = eyebrow.lower() + " " + title.lower()
        if "xemu" in low:
            kind = "Emulator (xemu)"
        elif "xbox hardware" in low or "original xbox" in low or "on the xbox" in low or "console" in low:
            kind = "Real console"
        else:
            kind = ""
        reports.append(dict(file=name, title=title, eyebrow=eyebrow, lede=lede, kind=kind, size=os.path.getsize(path)))
    # real-console reports first
    reports.sort(key=lambda r: (r["kind"] != "Real console", r["file"]))
    return reports


def performance_body(reports):
    items = []
    for i, r in enumerate(reports):
        src = "performance/reports/" + r["file"]
        tag = '<span class="tag">%s</span> ' % html.escape(r["kind"]) if r["kind"] else ""
        items.append("""<section class="report" id="r%d">
<h3>%s%s</h3>
<p>%s</p>
<p class="mocklinks"><button type="button" class="showreport" data-src="%s">Show it here</button>
<a href="%s">Open it in its own page</a> &middot;
<a href="%s/blob/main/docs/r3d/reports/%s">Source file</a></p>
<div class="reportframe"><iframe title="%s" loading="lazy"></iframe></div>
</section>""" % (i, tag, html.escape(r["title"]), html.escape(r["lede"]), src,
                 src, REPO, r["file"], html.escape(r["title"])))
    if not items:
        items.append("<p>No reports are published yet.</p>")
    return """
<h1>Performance</h1>
<p class="lede">Frame-time recordings from the Xbox build, as full interactive reports: per-map frame rate, where the
CPU time goes, and how each optimisation moved the numbers. The target and the list of changes are on the
<a href="progress.html">Progress</a> page.</p>

<div class="note"><strong>Real console or emulator.</strong> A report says which one it is. Console reports are from a
retail Xbox (733 MHz Pentium III). Emulator reports time xemu on a PC: the work counters in them are exact, the times
are not the console's.</div>

<h2>Reports</h2>
%s

<h2>Adding a report</h2>
<p>This page lists whatever is in <code>docs/r3d/reports/</code> when the site is built, so a new measurement is
published without editing the site:</p>
<ol>
<li>Record on the console with the <a href="recorder.html">frame-time recorder</a> and fetch the files:
<code>python tools/frametime-report.py fetch --host &lt;console IP&gt; --out recordings/ --label &lt;name&gt;</code></li>
<li>Build the report: <code>python tools/frametime-report.py report recordings/ --label &lt;name&gt; -o docs/r3d/reports/&lt;name&gt;.html</code></li>
<li>Commit it, and run the Pages workflow. The report appears here with its title and summary taken from the file.</li>
</ol>
<p>The step-by-step console report combines many recordings, so it is updated by editing its own file; a report made
by <code>frametime-report.py</code> needs nothing beyond the three steps.</p>
<script>
(function(){
var btns=document.querySelectorAll('.showreport');
for(var i=0;i<btns.length;i++){btns[i].addEventListener('click',function(){
  var sec=this.closest('.report'),box=sec.querySelector('.reportframe'),fr=box.querySelector('iframe');
  fr.src=this.getAttribute('data-src');box.classList.add('on');this.style.display='none';
});}
})();
</script>
""" % "\n".join(items)


page("performance.html", "Performance - Xash3D-FWGS-XBOX", performance_body(read_reports()),
     "Frame-time reports from the Xbox build: real-console and emulator recordings, step by step.")


# ---- Copy what the site links to but the repository keeps elsewhere -----------------------------

def copy_assets():
    rep_out = os.path.join(OUT, "performance", "reports")
    mock_out = os.path.join(OUT, "mocks")
    shutil.rmtree(rep_out, ignore_errors=True)
    os.makedirs(rep_out)
    os.makedirs(mock_out, exist_ok=True)
    for path in glob.glob(os.path.join(REPORTS_SRC, "*.html")):
        shutil.copy(path, rep_out)
    if os.path.exists(MOCK_SRC):
        shutil.copy(MOCK_SRC, os.path.join(mock_out, "half-life-xbox-menu.html"))


copy_assets()
print("ok")
