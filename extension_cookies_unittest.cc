// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Tests common functionality used by the Chrome Extensions Cookies API
// implementation.

#include "testing/gtest/include/gtest/gtest.h"

#include "chrome/browser/extensions/extension_cookies_api_constants.h"
#include "chrome/browser/extensions/extension_cookies_helpers.h"
#include "chrome/test/testing_profile.h"

namespace keys = extension_cookies_api_constants;

namespace {

struct DomainMatchCase {
  const char* filter;
  const char* domain;
  const bool matches;
};

// A test profile that supports linking with another profile for off-the-record
// (a.k.a. incognito) support.
class OtrTestingProfile : public TestingProfile {
 public:
  OtrTestingProfile() : linked_profile_(NULL) {}
  virtual Profile* GetOriginalProfile() {
    if (IsOffTheRecord())
      return linked_profile_;
    else
      return this;
  }

  virtual Profile* GetOffTheRecordProfile() {
    if (IsOffTheRecord())
      return this;
    else
      return linked_profile_;
  }

  static void LinkProfiles(OtrTestingProfile* profile1,
                           OtrTestingProfile* profile2) {
    profile1->set_linked_profile(profile2);
    profile2->set_linked_profile(profile1);
  }

  void set_linked_profile(OtrTestingProfile* profile) {
    linked_profile_ = profile;
  }

 private:
  OtrTestingProfile* linked_profile_;
};

}  // namespace

class ExtensionCookiesTest : public testing::Test {
};

TEST_F(ExtensionCookiesTest, StoreIdProfileConversion) {
  OtrTestingProfile profile, otrProfile;
  otrProfile.set_off_the_record(true);
  OtrTestingProfile::LinkProfiles(&profile, &otrProfile);

  EXPECT_EQ(std::string("0"),
            extension_cookies_helpers::GetStoreIdFromProfile(&profile));
  EXPECT_EQ(&profile,
            extension_cookies_helpers::ChooseProfileFromStoreId(
                "0", &profile, true));
  EXPECT_EQ(&profile,
            extension_cookies_helpers::ChooseProfileFromStoreId(
                "0", &profile, false));
  EXPECT_EQ(&otrProfile,
            extension_cookies_helpers::ChooseProfileFromStoreId(
                "1", &profile, true));
  EXPECT_EQ(NULL,
            extension_cookies_helpers::ChooseProfileFromStoreId(
                "1", &profile, false));

  EXPECT_EQ(std::string("1"),
            extension_cookies_helpers::GetStoreIdFromProfile(&otrProfile));
  EXPECT_EQ(&profile,
            extension_cookies_helpers::ChooseProfileFromStoreId(
                "0", &otrProfile, true));
  EXPECT_EQ(&profile,
            extension_cookies_helpers::ChooseProfileFromStoreId(
                "0", &otrProfile, false));
  EXPECT_EQ(&otrProfile,
            extension_cookies_helpers::ChooseProfileFromStoreId(
                "1", &otrProfile, true));
  EXPECT_EQ(NULL,
            extension_cookies_helpers::ChooseProfileFromStoreId(
                "1", &otrProfile, false));
}

TEST_F(ExtensionCookiesTest, ExtensionTypeCreation) {
  std::string string_value;
  bool boolean_value;
  double double_value;
  Value* value;

  net::CookieMonster::CanonicalCookie cookie1(
      "ABC", "DEF", "/", false, false,
      base::Time(), base::Time(), false, base::Time());
  net::CookieMonster::CookieListPair cookie_pair1("www.foobar.com", cookie1);
  scoped_ptr<DictionaryValue> cookie_value1(
      extension_cookies_helpers::CreateCookieValue(
          cookie_pair1, "some cookie store"));
  EXPECT_TRUE(cookie_value1->GetString(keys::kNameKey, &string_value));
  EXPECT_EQ("ABC", string_value);
  EXPECT_TRUE(cookie_value1->GetString(keys::kValueKey, &string_value));
  EXPECT_EQ("DEF", string_value);
  EXPECT_TRUE(cookie_value1->GetString(keys::kDomainKey, &string_value));
  EXPECT_EQ("www.foobar.com", string_value);
  EXPECT_TRUE(cookie_value1->GetBoolean(keys::kHostOnlyKey, &boolean_value));
  EXPECT_EQ(true, boolean_value);
  EXPECT_TRUE(cookie_value1->GetString(keys::kPathKey, &string_value));
  EXPECT_EQ("/", string_value);
  EXPECT_TRUE(cookie_value1->GetBoolean(keys::kSecureKey, &boolean_value));
  EXPECT_EQ(false, boolean_value);
  EXPECT_TRUE(cookie_value1->GetBoolean(keys::kHttpOnlyKey, &boolean_value));
  EXPECT_EQ(false, boolean_value);
  EXPECT_TRUE(cookie_value1->GetBoolean(keys::kSessionKey, &boolean_value));
  EXPECT_EQ(true, boolean_value);
  EXPECT_FALSE(
      cookie_value1->GetReal(keys::kExpirationDateKey, &double_value));
  EXPECT_TRUE(cookie_value1->GetString(keys::kStoreIdKey, &string_value));
  EXPECT_EQ("some cookie store", string_value);

  net::CookieMonster::CanonicalCookie cookie2(
      "ABC", "DEF", "/", false, false,
      base::Time(), base::Time(), true, base::Time::FromDoubleT(10000));
  net::CookieMonster::CookieListPair cookie_pair2(".foobar.com", cookie2);
  scoped_ptr<DictionaryValue> cookie_value2(
      extension_cookies_helpers::CreateCookieValue(
          cookie_pair2, "some cookie store"));
  EXPECT_TRUE(cookie_value2->GetBoolean(keys::kHostOnlyKey, &boolean_value));
  EXPECT_EQ(false, boolean_value);
  EXPECT_TRUE(cookie_value2->GetBoolean(keys::kSessionKey, &boolean_value));
  EXPECT_EQ(false, boolean_value);
  EXPECT_TRUE(cookie_value2->GetReal(keys::kExpirationDateKey, &double_value));
  EXPECT_EQ(10000, double_value);

  TestingProfile profile;
  ListValue* tab_ids = new ListValue();
  scoped_ptr<DictionaryValue> cookie_store_value(
      extension_cookies_helpers::CreateCookieStoreValue(&profile, tab_ids));
  EXPECT_TRUE(cookie_store_value->GetString(keys::kIdKey, &string_value));
  EXPECT_EQ("0", string_value);
  EXPECT_TRUE(cookie_store_value->Get(keys::kTabIdsKey, &value));
  EXPECT_EQ(tab_ids, value);
}

TEST_F(ExtensionCookiesTest, GetURLFromCookiePair) {
  net::CookieMonster::CanonicalCookie cookie1(
      "ABC", "DEF", "/", false, false,
      base::Time(), base::Time(), false, base::Time());
  net::CookieMonster::CookieListPair cookie_pair1("www.foobar.com", cookie1);
  EXPECT_EQ("http://www.foobar.com/",
            extension_cookies_helpers::GetURLFromCookiePair(
                cookie_pair1).spec());

  net::CookieMonster::CanonicalCookie cookie2(
      "ABC", "DEF", "/", true, false,
      base::Time(), base::Time(), false, base::Time());
  net::CookieMonster::CookieListPair cookie_pair2(".helloworld.com", cookie2);
  EXPECT_EQ("https://helloworld.com/",
            extension_cookies_helpers::GetURLFromCookiePair(
                cookie_pair2).spec());
}

TEST_F(ExtensionCookiesTest, EmptyDictionary) {
  scoped_ptr<DictionaryValue> details(new DictionaryValue());
  extension_cookies_helpers::MatchFilter filter(details.get());
  std::string domain;
  net::CookieMonster::CanonicalCookie cookie;
  net::CookieMonster::CookieListPair cookie_pair(domain, cookie);

  EXPECT_TRUE(filter.MatchesCookie(cookie_pair));
}

TEST_F(ExtensionCookiesTest, DomainMatching) {
  const DomainMatchCase tests[] = {
    { "bar.com", "bar.com", true },
    { ".bar.com", "bar.com", true },
    { "bar.com", "foo.bar.com", true },
    { "bar.com", "bar.foo.com", false },
    { ".bar.com", ".foo.bar.com", true },
    { ".bar.com", "baz.foo.bar.com", true },
    { "foo.bar.com", ".bar.com", false }
  };

  scoped_ptr<DictionaryValue> details(new DictionaryValue());
  net::CookieMonster::CanonicalCookie cookie;
  for (size_t i = 0; i < arraysize(tests); ++i) {
    details->SetString(keys::kDomainKey, std::string(tests[i].filter));
    extension_cookies_helpers::MatchFilter filter(details.get());
    net::CookieMonster::CookieListPair cookie_pair(tests[i].domain, cookie);
    EXPECT_EQ(tests[i].matches, filter.MatchesCookie(cookie_pair));
  }
}
