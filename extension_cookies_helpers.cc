// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implements common functionality for the Chrome Extensions Cookies API.

#include "chrome/browser/extensions/extension_cookies_helpers.h"

#include "base/logging.h"
#include "chrome/browser/browser.h"
#include "chrome/browser/extensions/extension_cookies_api_constants.h"
#include "chrome/browser/extensions/extension_tabs_module.h"
#include "chrome/browser/profile.h"
#include "chrome/common/extensions/extension.h"
#include "chrome/common/url_constants.h"
#include "googleurl/src/gurl.h"

namespace keys = extension_cookies_api_constants;

namespace extension_cookies_helpers {

static const char kOriginalProfileStoreId[] = "0";
static const char kOffTheRecordProfileStoreId[] = "1";

Profile* ChooseProfileFromStoreId(const std::string& store_id,
                                  Profile* profile,
                                  bool include_incognito) {
  DCHECK(profile);
  if (store_id == kOriginalProfileStoreId)
    return profile->GetOriginalProfile();
  if (store_id == kOffTheRecordProfileStoreId && include_incognito)
    return profile->GetOffTheRecordProfile();
  return NULL;
}

const char* GetStoreIdFromProfile(Profile* profile) {
  DCHECK(profile);
  return profile->IsOffTheRecord() ?
      kOffTheRecordProfileStoreId : kOriginalProfileStoreId;
}

DictionaryValue* CreateCookieValue(
    const net::CookieMonster::CookieListPair& cookie_pair,
    const std::string& store_id) {
  DictionaryValue* result = new DictionaryValue();

  const net::CookieMonster::CanonicalCookie& cookie = cookie_pair.second;
  result->SetString(keys::kNameKey, cookie.Name());
  result->SetString(keys::kValueKey, cookie.Value());
  result->SetString(keys::kDomainKey, cookie_pair.first);
  result->SetBoolean(keys::kHostOnlyKey,
                     net::CookieMonster::DomainIsHostOnly(cookie_pair.first));
  result->SetString(keys::kPathKey, cookie.Path());
  result->SetBoolean(keys::kSecureKey, cookie.IsSecure());
  result->SetBoolean(keys::kHttpOnlyKey, cookie.IsHttpOnly());
  result->SetBoolean(keys::kSessionKey, !cookie.DoesExpire());
  if (cookie.DoesExpire())
    result->SetReal(keys::kExpirationDateKey, cookie.ExpiryDate().ToDoubleT());
  result->SetString(keys::kStoreIdKey, store_id);

  return result;
}

DictionaryValue* CreateCookieStoreValue(Profile* profile,
                                        ListValue* tab_ids) {
  DCHECK(profile);
  DCHECK(tab_ids);
  DictionaryValue* result = new DictionaryValue();
  result->SetString(keys::kIdKey, GetStoreIdFromProfile(profile));
  result->Set(keys::kTabIdsKey, tab_ids);
  return result;
}

net::CookieMonster::CookieList GetCookieListFromStore(
    net::CookieStore* cookie_store, const GURL& url) {
  DCHECK(cookie_store);
  net::CookieMonster* monster = cookie_store->GetCookieMonster();
  if (!url.is_empty()) {
    DCHECK(url.is_valid());
    return monster->GetAllCookiesForURL(url);
  }
  return monster->GetAllCookies();
}

GURL GetURLFromCookiePair(
    const net::CookieMonster::CookieListPair& cookie_pair) {
  const std::string& domain_key = cookie_pair.first;
  const net::CookieMonster::CanonicalCookie& cookie = cookie_pair.second;
  const std::string scheme =
      cookie.IsSecure() ? chrome::kHttpsScheme : chrome::kHttpScheme;
  const std::string host =
      domain_key.find('.') != 0 ? domain_key : domain_key.substr(1);
  return GURL(scheme + chrome::kStandardSchemeSeparator + host + "/");
}

void AppendMatchingCookiesToList(
    net::CookieStore* cookie_store, const std::string& store_id,
    const GURL& url, const DictionaryValue* details,
    const Extension* extension,
    ListValue* match_list) {
  net::CookieMonster::CookieList all_cookies = GetCookieListFromStore(
      cookie_store, url);
  net::CookieMonster::CookieList::const_iterator it;
  for (it = all_cookies.begin(); it != all_cookies.end(); ++it) {
    // Ignore any cookie whose domain doesn't match the extension's
    // host permissions.
    GURL cookie_domain_url = GetURLFromCookiePair(*it);
    if (!extension->HasHostPermission(cookie_domain_url))
      continue;
    // Filter the cookie using the match filter.
    extension_cookies_helpers::MatchFilter filter(details);
    if (filter.MatchesCookie(*it))
      match_list->Append(CreateCookieValue(*it, store_id));
  }
}

void AppendToTabIdList(Browser* browser, ListValue* tab_ids) {
  DCHECK(browser);
  DCHECK(tab_ids);
  TabStripModel* tab_strip = browser->tabstrip_model();
  for (int i = 0; i < tab_strip->count(); ++i) {
    tab_ids->Append(Value::CreateIntegerValue(
        ExtensionTabUtil::GetTabId(tab_strip->GetTabContentsAt(i))));
  }
}

MatchFilter::MatchFilter(const DictionaryValue* details)
    : details_(details) {
  DCHECK(details_);
}

bool MatchFilter::MatchesCookie(
    const net::CookieMonster::CookieListPair& cookie_pair) {
  const net::CookieMonster::CanonicalCookie& cookie = cookie_pair.second;
  return
      MatchesString(keys::kNameKey, cookie.Name()) &&
      MatchesDomain(cookie_pair.first) &&
      MatchesString(keys::kPathKey, cookie.Path()) &&
      MatchesBoolean(keys::kSecureKey, cookie.IsSecure()) &&
      MatchesBoolean(keys::kSessionKey, !cookie.DoesExpire());
}

bool MatchFilter::MatchesString(const wchar_t* key, const std::string& value) {
  if (!details_->HasKey(key))
    return true;
  std::string filter_value;
  return (details_->GetString(key, &filter_value) &&
          value == filter_value);
}

bool MatchFilter::MatchesBoolean(const wchar_t* key, bool value) {
  if (!details_->HasKey(key))
    return true;
  bool filter_value = false;
  return (details_->GetBoolean(key, &filter_value) &&
          value == filter_value);
}

bool MatchFilter::MatchesDomain(const std::string& domain) {
  if (!details_->HasKey(keys::kDomainKey))
    return true;

  std::string filter_value;
  if (!details_->GetString(keys::kDomainKey, &filter_value))
    return false;
  // Add a leading '.' character to the filter domain if it doesn't exist.
  if (net::CookieMonster::DomainIsHostOnly(filter_value))
    filter_value.insert(0, ".");

  std::string sub_domain(domain);
  // Strip any leading '.' character from the input cookie domain.
  if (!net::CookieMonster::DomainIsHostOnly(sub_domain))
    sub_domain = sub_domain.substr(1);

  // Now check whether the domain argument is a subdomain of the filter domain.
  for (sub_domain.insert(0, ".");
       sub_domain.length() >= filter_value.length();) {
    if (sub_domain == filter_value)
      return true;
    const size_t next_dot = sub_domain.find('.', 1);  // Skip over leading dot.
    sub_domain.erase(0, next_dot);
  }
  return false;
}

}  // namespace extension_cookies_helpers
