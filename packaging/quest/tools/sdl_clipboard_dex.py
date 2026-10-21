#!/usr/bin/env python3
"""Write an APK's classes.dex with SDL's clipboard handler patched for a
missing clipboard service.

usage: sdl_clipboard_dex.py base.apk out.dex

Steam Frame: its Android layer (Lepton) has no clipboard service, so
getSystemService(CLIPBOARD_SERVICE) returns null and SDL's activity crashes in
SDLClipboardHandler's constructor at startup. The patched class tolerates a
null ClipboardManager: the constructor adds its listener only when there is
one, clipboardHasText returns false, clipboardGetText null, clipboardSetText
does nothing. With a clipboard (Quest, Pico) nothing changes.

The repack builds (package_tie_quest.sh, and OpenXWA's release packaging) take
classes.dex from an earlier APK instead of compiling SDL's Java, so the patch
is made on its smali: Apktool's baksmali/smali (APKTOOL, default
/root/android/apktool_2.12.1.jar) take the dex apart and put it back together.
build_apk.py patches the Java source the same way (SDL_CLIPBOARD_PATCHES);
keep the two in step.

Stops with an error when the expected smali is not found (another SDL), and
checks the result: disassembled again, every other class is unchanged and the
handler has its guards.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

CLS = 'smali/org/libsdl/app/SDLClipboardHandler.smali'
FIELD = 'Lorg/libsdl/app/SDLClipboardHandler;->mClipMgr:Landroid/content/ClipboardManager;'
ADD = ('invoke-virtual {v0, p0}, Landroid/content/ClipboardManager;->addPrimaryClipChangedListener'
       '(Landroid/content/ClipboardManager$OnPrimaryClipChangedListener;)V')


def guard(head, locals_before, ret):
    """The method header with a null check of mClipMgr in front of its body."""
    old = '%s\n    .locals %d\n' % (head, locals_before)
    new = ('%s\n    .locals %d\n\n'
           '    iget-object v0, p0, %s\n\n'
           '    if-nez v0, :sf_has_clipboard\n\n'
           '%s\n'
           '    :sf_has_clipboard\n') % (head, max(locals_before, 1), FIELD, ret)
    return old, new


PATCHES = [
    # Constructor: add the listener only when there is a ClipboardManager.
    ('    %s\n\n    return-void\n' % ADD,
     '    if-eqz v0, :sf_no_clipboard\n\n    %s\n\n    :sf_no_clipboard\n    return-void\n' % ADD),
    guard('.method public clipboardHasText()Z', 0, '    const/4 v0, 0x0\n\n    return v0\n'),
    guard('.method public clipboardGetText()Ljava/lang/String;', 1, '    const/4 v0, 0x0\n\n    return-object v0\n'),
    guard('.method public clipboardSetText(Ljava/lang/String;)V', 1, '    return-void\n'),
]


def patch_smali(text):
    for old, new in PATCHES:
        n = text.count(old)
        if n != 1:
            sys.exit('SDLClipboardHandler.smali: the Steam Frame clipboard patch expects %r once, found it %d '
                     'times; update PATCHES in sdl_clipboard_dex.py' % (old.strip().splitlines()[0], n))
        text = text.replace(old, new)
    return text


def methods(text):
    return dict((m.group(1), m.group(2)) for m in
                re.finditer(r'^\.method [^\n]*?(\S+)\n(.*?)^\.end method', text, re.M | re.S))


def check_patched(text):
    """The re-disassembled handler (labels renamed by baksmali) has the guards."""
    ms = methods(text)
    init = ms.get('<init>()V', '')
    i, j = init.find('if-eqz v0'), init.find('addPrimaryClipChangedListener')
    if not (0 <= i < j):
        sys.exit('check failed: the constructor does not test mClipMgr before adding the listener')
    for name in ('clipboardHasText()Z', 'clipboardGetText()Ljava/lang/String;', 'clipboardSetText(Ljava/lang/String;)V'):
        body = [l.strip() for l in ms.get(name, '').splitlines()
                if l.strip() and not l.strip().startswith(('.locals', '.line', '.param', '.end param'))]
        if body[:2] != ['iget-object v0, p0, ' + FIELD, body[1]] or not body[1].startswith('if-nez v0, :'):
            sys.exit('check failed: %s does not start with the mClipMgr null check' % name)


def apktool(jar, *args):
    env = dict(os.environ, JAVA_TOOL_OPTIONS='')
    r = subprocess.run(['java', '-jar', jar] + list(args), env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        sys.exit('apktool %s failed:\n%s' % (args[0], r.stdout))


def decode(jar, dex, manifest, work, name):
    """Disassemble dex (with the base's binary manifest, as Apktool wants an APK)."""
    apk = os.path.join(work, name + '.apk')
    with zipfile.ZipFile(apk, 'w') as z:
        z.writestr('classes.dex', dex)
        z.writestr('AndroidManifest.xml', manifest)
    out = os.path.join(work, name)
    apktool(jar, 'd', '-r', '-f', '-o', out, apk)
    return out


def tree(root):
    files = {}
    for d, _, names in os.walk(os.path.join(root, 'smali')):
        for n in names:
            p = os.path.join(d, n)
            files[os.path.relpath(p, root)] = open(p, 'rb').read()
    return files


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.split('\n\n')[1])
    base, out = sys.argv[1:3]
    jar = os.environ.get('APKTOOL', '/root/android/apktool_2.12.1.jar')
    if not os.path.isfile(jar):
        sys.exit('Apktool not found: %s (set APKTOOL)' % jar)
    with zipfile.ZipFile(base) as z:
        dex, manifest = z.read('classes.dex'), z.read('AndroidManifest.xml')
    work = tempfile.mkdtemp(prefix='sdl-clipboard-dex-')
    try:
        src = decode(jar, dex, manifest, work, 'orig')
        orig = tree(src)
        cls = os.path.join(src, CLS)
        with open(cls) as f:
            text = f.read()
        with open(cls, 'w') as f:
            f.write(patch_smali(text))
        built = os.path.join(work, 'patched.apk')
        apktool(jar, 'b', '-o', built, src)
        with zipfile.ZipFile(built) as z:
            new_dex = z.read('classes.dex')
        if new_dex[:8] != dex[:8]:
            sys.exit('dex version changed: %r -> %r' % (dex[:8], new_dex[:8]))
        again = tree(decode(jar, new_dex, manifest, work, 'check'))
        if sorted(again) != sorted(orig):
            sys.exit('check failed: the class list changed')
        changed = [n for n in orig if orig[n] != again[n]]
        if changed != [CLS]:
            sys.exit('check failed: changed classes %s, expected only %s' % (changed, CLS))
        check_patched(again[CLS].decode())
        with open(out, 'wb') as f:
            f.write(new_dex)
        print('%s: %d bytes, SDLClipboardHandler patched (null clipboard tolerated), %d other classes unchanged'
              % (out, len(new_dex), len(orig) - 1))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
