"""Shared fresh-profile Firefox source; callers substitute HOME and URL literals."""

BROWSER_SOURCE = """import os
from pathlib import Path
home=Path(HOME);profile=home/'profile';profile.mkdir(parents=True,exist_ok=False)
prefs={'browser.startup.homepage_override.mstone':'ignore','browser.shell.checkDefaultBrowser':False,'browser.startup.page':0,'browser.newtabpage.enabled':False,'startup.homepage_welcome_url':'','datareporting.policy.dataSubmissionPolicyBypassNotification':True,'browser.sessionstore.resume_from_crash':False,'media.autoplay.default':0,'media.autoplay.blocking_policy':0}
import json
(profile/'user.js').write_text(''.join('user_pref('+json.dumps(k)+','+json.dumps(v)+');'+chr(10) for k,v in prefs.items()))
for name in ('config','cache','data','tmp'):(home/name).mkdir()
os.environ.update(HOME=str(home),XDG_CONFIG_HOME=str(home/'config'),XDG_CACHE_HOME=str(home/'cache'),XDG_DATA_HOME=str(home/'data'),TMPDIR=str(home/'tmp'),MOZ_ENABLE_WAYLAND='1',GDK_BACKEND='wayland',MOZ_SHM_NO_SEALS='1')
os.execv('/nix/profile/bin/brook-run',['/nix/profile/bin/brook-run','/nix/profile/bin/firefox','--no-remote','--profile',str(profile),URL])
"""
