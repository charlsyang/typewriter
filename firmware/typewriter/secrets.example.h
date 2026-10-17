// Copy this file to secrets.h (which git ignores) and fill it in.
// tools/dropbox_auth.py writes the two Dropbox values for you.
#pragma once

#define WIFI_SSID "your network name"
#define WIFI_PASSWORD "your network password"

#define DROPBOX_APP_KEY ""
#define DROPBOX_REFRESH_TOKEN ""

// Readwise access token, from readwise.io/access_token
#define READWISE_TOKEN ""

// Dropbox folders (optional; these are the defaults). Daily notes go in
// DROPBOX_DAILY_NOTES as YYYY-MM-DD.md (keep the trailing slash); drafts are
// the .md/.mdx files in DROPBOX_ESSAYS without isPublished: true (no slash).
#define DROPBOX_DAILY_NOTES "/OS/Daily Notes/"
#define DROPBOX_ESSAYS "/Site/blog"
