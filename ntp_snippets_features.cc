// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ntp_snippets/ntp_snippets_features.h"

const base::Feature kContentSuggestionsNotificationsFeature = {
    "ContentSuggestionsNotifications", base::FEATURE_DISABLED_BY_DEFAULT};

const char kContentSuggestionsNotificationsAlwaysNotifyParam[] =
    "always_notify";
const char kContentSuggestionsNotificationsUseSnippetAsTextParam[] =
    "use_snippet_as_text";
const char
    kContentSuggestionsNotificationsKeepNotificationWhenFrontmostParam[] =
        "keep_notification_when_frontmost";
const char kContentSuggestionsNotificationsDailyLimit[] = "daily_limit";
const char kContentSuggestionsNotificationsIgnoredLimitParam[] =
    "ignored_limit";
