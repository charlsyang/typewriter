#!/usr/bin/env python3
"""Connect the typewriter to your Dropbox account.

Uses Dropbox's OAuth flow for apps without a secret (PKCE): you approve the app
in your browser, paste the code it shows back here, and this script saves a
long-lived refresh token into firmware/typewriter/secrets.h. The token is never
printed.

Usage: tools/dropbox_auth.py APP_KEY
"""
import base64
import hashlib
import json
import os
import re
import secrets
import sys
import urllib.parse
import urllib.request
import webbrowser

SECRETS = os.path.join(os.path.dirname(__file__), "..", "firmware", "typewriter", "secrets.h")
TEMPLATE = os.path.join(os.path.dirname(__file__), "..", "firmware", "typewriter", "secrets.example.h")
SCOPES = "files.metadata.read files.content.read files.content.write"


def set_define(text, name, value):
    line = f'#define {name} "{value}"'
    pattern = re.compile(rf"^#define {name} .*$", re.M)
    return pattern.sub(line, text) if pattern.search(text) else text.rstrip("\n") + "\n" + line + "\n"


def main():
    if len(sys.argv) != 2:
        sys.exit("Usage: tools/dropbox_auth.py APP_KEY")
    app_key = sys.argv[1].strip()

    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    url = "https://www.dropbox.com/oauth2/authorize?" + urllib.parse.urlencode({
        "client_id": app_key,
        "response_type": "code",
        "code_challenge": challenge,
        "code_challenge_method": "S256",
        "token_access_type": "offline",
        "scope": SCOPES,
    })
    print("Opening Dropbox in your browser. Approve the app, then copy the code it shows.")
    print(f"If the browser doesn't open, visit:\n{url}\n")
    webbrowser.open(url)
    code = input("Paste the code here: ").strip()

    body = urllib.parse.urlencode({
        "code": code,
        "grant_type": "authorization_code",
        "code_verifier": verifier,
        "client_id": app_key,
    }).encode()
    try:
        with urllib.request.urlopen("https://api.dropboxapi.com/oauth2/token", body) as resp:
            token = json.load(resp)
    except urllib.error.HTTPError as e:
        sys.exit(f"Dropbox refused the code ({e.code}): {e.read().decode(errors='replace')}")
    refresh = token.get("refresh_token")
    if not refresh:
        sys.exit("Dropbox didn't return a refresh token.")

    if os.path.exists(SECRETS):
        text = open(SECRETS).read()
    else:
        text = open(TEMPLATE).read()
    text = set_define(text, "DROPBOX_APP_KEY", app_key)
    text = set_define(text, "DROPBOX_REFRESH_TOKEN", refresh)
    with open(SECRETS, "w") as f:
        f.write(text)
    os.chmod(SECRETS, 0o600)
    print(f"Connected as account {token.get('account_id', '?')}. Saved to firmware/typewriter/secrets.h.")
    if 'your network name' in text:
        print("Next: put your Wi-Fi name and password in that file.")


if __name__ == "__main__":
    main()
