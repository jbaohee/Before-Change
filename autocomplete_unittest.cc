// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/bind.h"
#include "base/memory/scoped_ptr.h"
#include "base/message_loop.h"
#include "base/string16.h"
#include "base/string_number_conversions.h"
#include "base/string_util.h"
#include "base/utf_string_conversions.h"
#include "chrome/browser/autocomplete/autocomplete.h"
#include "chrome/browser/autocomplete/autocomplete_match.h"
#include "chrome/browser/autocomplete/autocomplete_provider.h"
#include "chrome/browser/autocomplete/autocomplete_provider_listener.h"
#include "chrome/browser/autocomplete/keyword_provider.h"
#include "chrome/browser/autocomplete/search_provider.h"
#include "chrome/browser/search_engines/template_url.h"
#include "chrome/browser/search_engines/template_url_service.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/common/chrome_notification_types.h"
#include "chrome/test/base/testing_browser_process.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/notification_source.h"
#include "testing/gtest/include/gtest/gtest.h"

static std::ostream& operator<<(std::ostream& os,
                                const AutocompleteResult::const_iterator& it) {
  return os << static_cast<const AutocompleteMatch*>(&(*it));
}

namespace {
const size_t kResultsPerProvider = 3;
const char kTestTemplateURLKeyword[] = "t";
}

// Autocomplete provider that provides known results. Note that this is
// refcounted so that it can also be a task on the message loop.
class TestProvider : public AutocompleteProvider {
 public:
  TestProvider(int relevance, const string16& prefix,
               Profile* profile,
               const string16 match_keyword)
      : AutocompleteProvider(NULL, profile, ""),
        relevance_(relevance),
        prefix_(prefix),
        match_keyword_(match_keyword) {
  }

  virtual void Start(const AutocompleteInput& input,
                     bool minimal_changes);

  void set_listener(AutocompleteProviderListener* listener) {
    listener_ = listener;
  }

 private:
  ~TestProvider() {}

  void Run();

  void AddResults(int start_at, int num);
  void AddResultsWithSearchTermsArgs(
      int start_at,
      int num,
      AutocompleteMatch::Type type,
      const TemplateURLRef::SearchTermsArgs& search_terms_args);

  int relevance_;
  const string16 prefix_;
  const string16 match_keyword_;
};

void TestProvider::Start(const AutocompleteInput& input,
                         bool minimal_changes) {
  if (minimal_changes)
    return;

  matches_.clear();

  // Generate 4 results synchronously, the rest later.
  AddResults(0, 1);
  AddResultsWithSearchTermsArgs(
      1, 1, AutocompleteMatch::SEARCH_WHAT_YOU_TYPED,
      TemplateURLRef::SearchTermsArgs(ASCIIToUTF16("echo")));
  AddResultsWithSearchTermsArgs(
      2, 1, AutocompleteMatch::NAVSUGGEST,
      TemplateURLRef::SearchTermsArgs(ASCIIToUTF16("nav")));
  AddResultsWithSearchTermsArgs(
      3, 1, AutocompleteMatch::SEARCH_SUGGEST,
      TemplateURLRef::SearchTermsArgs(ASCIIToUTF16("query")));

  if (input.matches_requested() == AutocompleteInput::ALL_MATCHES) {
    done_ = false;
    MessageLoop::current()->PostTask(FROM_HERE, base::Bind(&TestProvider::Run,
                                                           this));
  }
}

void TestProvider::Run() {
  DCHECK_GT(kResultsPerProvider, 0U);
  AddResults(1, kResultsPerProvider);
  done_ = true;
  DCHECK(listener_);
  listener_->OnProviderUpdate(true);
}

void TestProvider::AddResults(int start_at, int num) {
  AddResultsWithSearchTermsArgs(start_at,
                                num,
                                AutocompleteMatch::URL_WHAT_YOU_TYPED,
                                TemplateURLRef::SearchTermsArgs(string16()));
}

void TestProvider::AddResultsWithSearchTermsArgs(
    int start_at,
    int num,
    AutocompleteMatch::Type type,
    const TemplateURLRef::SearchTermsArgs& search_terms_args) {
  for (int i = start_at; i < num; i++) {
    AutocompleteMatch match(this, relevance_ - i, false, type);

    match.fill_into_edit = prefix_ + UTF8ToUTF16(base::IntToString(i));
    match.destination_url = GURL(UTF16ToUTF8(match.fill_into_edit));

    match.contents = match.fill_into_edit;
    match.contents_class.push_back(
        ACMatchClassification(0, ACMatchClassification::NONE));
    match.description = match.fill_into_edit;
    match.description_class.push_back(
        ACMatchClassification(0, ACMatchClassification::NONE));
    match.search_terms_args.reset(
        new TemplateURLRef::SearchTermsArgs(search_terms_args));
    if (!match_keyword_.empty()) {
      match.keyword = match_keyword_;
      ASSERT_TRUE(match.GetTemplateURL(profile_) != NULL);
    }

    matches_.push_back(match);
  }
}

class AutocompleteProviderTest : public testing::Test,
                                 public content::NotificationObserver {
 protected:
  struct KeywordTestData {
    const string16 fill_into_edit;
    const string16 keyword;
    const bool expected_keyword_result;
  };

  struct AssistedQueryStatsTestData {
    const AutocompleteMatch::Type match_type;
    const std::string expected_aqs;
  };

 protected:
   // Registers a test TemplateURL under the given keyword.
  void RegisterTemplateURL(const string16 keyword,
                           const std::string& template_url);

  void ResetControllerWithTestProviders(bool same_destinations);

  // Runs a query on the input "a", and makes sure both providers' input is
  // properly collected.
  void RunTest();

  void RunRedundantKeywordTest(const KeywordTestData* match_data, size_t size);

  void RunAssistedQueryStatsTest(
      const AssistedQueryStatsTestData* aqs_test_data,
      size_t size);

  void RunQuery(const string16 query);

  void ResetControllerWithTestProvidersWithKeywordAndSearchProviders();
  void ResetControllerWithKeywordProvider();
  void RunExactKeymatchTest(bool allow_exact_keyword_match);

  // These providers are owned by the controller once it's created.
  ACProviders providers_;

  AutocompleteResult result_;
  scoped_ptr<AutocompleteController> controller_;

 private:
  // content::NotificationObserver
  virtual void Observe(int type,
                       const content::NotificationSource& source,
                       const content::NotificationDetails& details);

  MessageLoopForUI message_loop_;
  content::NotificationRegistrar registrar_;
  TestingProfile profile_;
};

void AutocompleteProviderTest:: RegisterTemplateURL(
    const string16 keyword,
    const std::string& template_url) {
  TemplateURLServiceFactory::GetInstance()->SetTestingFactoryAndUse(
      &profile_, &TemplateURLServiceFactory::BuildInstanceFor);
  TemplateURLData data;
  data.SetURL(template_url);
  data.SetKeyword(keyword);
  TemplateURL* default_t_url = new TemplateURL(&profile_, data);
  TemplateURLService* turl_model =
      TemplateURLServiceFactory::GetForProfile(&profile_);
  turl_model->Add(default_t_url);
  turl_model->SetDefaultSearchProvider(default_t_url);
  TemplateURLID default_provider_id = default_t_url->id();
  ASSERT_NE(0, default_provider_id);
}

void AutocompleteProviderTest::ResetControllerWithTestProviders(
    bool same_destinations) {
  // Forget about any existing providers.  The controller owns them and will
  // Release() them below, when we delete it during the call to reset().
  providers_.clear();

  // TODO: Move it outside this method, after refactoring the existing
  // unit tests.  Specifically:
  //   (1) Make sure that AutocompleteMatch.keyword is set iff there is
  //       a corresponding call to RegisterTemplateURL; otherwise the
  //       controller flow will crash; this practically means that
  //       RunTests/ResetControllerXXX/RegisterTemplateURL should
  //       be coordinated with each other.
  //   (2) Inject test arguments rather than rely on the hardcoded values, e.g.
  //       don't rely on kResultsPerProvided and default relevance ordering
  //       (B > A).
  RegisterTemplateURL(ASCIIToUTF16(kTestTemplateURLKeyword),
                      "http://aqs/{searchTerms}/{google:assistedQueryStats}");

  // Construct two new providers, with either the same or different prefixes.
  TestProvider* providerA =
      new TestProvider(kResultsPerProvider,
                       ASCIIToUTF16("http://a"),
                       &profile_,
                       ASCIIToUTF16(kTestTemplateURLKeyword));
  providerA->AddRef();
  providers_.push_back(providerA);

  TestProvider* providerB = new TestProvider(
      kResultsPerProvider * 2,
      same_destinations ? ASCIIToUTF16("http://a") : ASCIIToUTF16("http://b"),
      &profile_,
      string16());
  providerB->AddRef();
  providers_.push_back(providerB);

  // Reset the controller to contain our new providers.
  AutocompleteController* controller =
      new AutocompleteController(providers_, &profile_);
  controller_.reset(controller);
  providerA->set_listener(controller);
  providerB->set_listener(controller);

  // The providers don't complete synchronously, so listen for "result updated"
  // notifications.
  registrar_.Add(this,
                 chrome::NOTIFICATION_AUTOCOMPLETE_CONTROLLER_RESULT_READY,
                 content::Source<AutocompleteController>(controller));
}

void AutocompleteProviderTest::
    ResetControllerWithTestProvidersWithKeywordAndSearchProviders() {
  TemplateURLServiceFactory::GetInstance()->SetTestingFactoryAndUse(
      &profile_, &TemplateURLServiceFactory::BuildInstanceFor);

  // Reset the default TemplateURL.
  TemplateURLData data;
  data.SetURL("http://defaultturl/{searchTerms}");
  TemplateURL* default_t_url = new TemplateURL(&profile_, data);
  TemplateURLService* turl_model =
      TemplateURLServiceFactory::GetForProfile(&profile_);
  turl_model->Add(default_t_url);
  turl_model->SetDefaultSearchProvider(default_t_url);
  TemplateURLID default_provider_id = default_t_url->id();
  ASSERT_NE(0, default_provider_id);

  // Create another TemplateURL for KeywordProvider.
  data.short_name = ASCIIToUTF16("k");
  data.SetKeyword(ASCIIToUTF16("k"));
  data.SetURL("http://keyword/{searchTerms}");
  TemplateURL* keyword_t_url = new TemplateURL(&profile_, data);
  turl_model->Add(keyword_t_url);
  ASSERT_NE(0, keyword_t_url->id());

  // Forget about any existing providers.  The controller owns them and will
  // Release() them below, when we delete it during the call to reset().
  providers_.clear();

  // Create both a keyword and search provider, and add them in that order.
  // (Order is important; see comments in RunExactKeymatchTest().)
  AutocompleteProvider* keyword_provider = new KeywordProvider(NULL, &profile_);
  keyword_provider->AddRef();
  providers_.push_back(keyword_provider);
  AutocompleteProvider* search_provider = new SearchProvider(NULL, &profile_);
  search_provider->AddRef();
  providers_.push_back(search_provider);

  controller_.reset(new AutocompleteController(providers_, &profile_));
}

void AutocompleteProviderTest::ResetControllerWithKeywordProvider() {
  TemplateURLServiceFactory::GetInstance()->SetTestingFactoryAndUse(
      &profile_, &TemplateURLServiceFactory::BuildInstanceFor);

  TemplateURLService* turl_model =
      TemplateURLServiceFactory::GetForProfile(&profile_);

  // Create a TemplateURL for KeywordProvider.
  TemplateURLData data;
  data.short_name = ASCIIToUTF16("foo.com");
  data.SetKeyword(ASCIIToUTF16("foo.com"));
  data.SetURL("http://foo.com/{searchTerms}");
  TemplateURL* keyword_t_url = new TemplateURL(&profile_, data);
  turl_model->Add(keyword_t_url);
  ASSERT_NE(0, keyword_t_url->id());

  // Create another TemplateURL for KeywordProvider.
  data.short_name = ASCIIToUTF16("bar.com");
  data.SetKeyword(ASCIIToUTF16("bar.com"));
  data.SetURL("http://bar.com/{searchTerms}");
  keyword_t_url = new TemplateURL(&profile_, data);
  turl_model->Add(keyword_t_url);
  ASSERT_NE(0, keyword_t_url->id());

  // Forget about any existing providers.  The controller owns them and will
  // Release() them below, when we delete it during the call to reset().
  providers_.clear();

  // Create both a keyword and search provider, and add them in that order.
  // (Order is important; see comments in RunExactKeymatchTest().)
  KeywordProvider* keyword_provider = new KeywordProvider(NULL, &profile_);
  keyword_provider->AddRef();
  providers_.push_back(keyword_provider);

  AutocompleteController* controller =
      new AutocompleteController(providers_, &profile_);
  controller->set_keyword_provider(keyword_provider);
  controller_.reset(controller);
}

void AutocompleteProviderTest::RunTest() {
  RunQuery(ASCIIToUTF16("a"));
}

void AutocompleteProviderTest::RunRedundantKeywordTest(
    const KeywordTestData* match_data,
    size_t size) {
  ACMatches matches;
  for (size_t i = 0; i < size; ++i) {
    AutocompleteMatch match;
    match.fill_into_edit = match_data[i].fill_into_edit;
    match.transition = content::PAGE_TRANSITION_KEYWORD;
    match.keyword = match_data[i].keyword;
    matches.push_back(match);
  }

  AutocompleteResult result;
  result.AppendMatches(matches);
  controller_->UpdateAssociatedKeywords(&result);

  for (size_t j = 0; j < result.size(); ++j) {
    EXPECT_EQ(match_data[j].expected_keyword_result,
        result.match_at(j)->associated_keyword.get() != NULL);
  }
}

void AutocompleteProviderTest::RunAssistedQueryStatsTest(
    const AssistedQueryStatsTestData* aqs_test_data,
    size_t size) {
  // Prepare input.
  const size_t kMaxRelevance = 1000;
  ACMatches matches;
  for (size_t i = 0; i < size; ++i) {
    AutocompleteMatch match(NULL, kMaxRelevance - i, false,
                            aqs_test_data[i].match_type);
    match.keyword = ASCIIToUTF16(kTestTemplateURLKeyword);
    match.search_terms_args.reset(
        new TemplateURLRef::SearchTermsArgs(string16()));
    matches.push_back(match);
  }
  result_.Reset();
  result_.AppendMatches(matches);

  // Update AQS.
  controller_->UpdateAssistedQueryStats(&result_);

  // Verify data.
  for (size_t i = 0; i < size; ++i) {
    EXPECT_EQ(aqs_test_data[i].expected_aqs,
              result_.match_at(i)->search_terms_args->assisted_query_stats);
  }
}

void AutocompleteProviderTest::RunQuery(const string16 query) {
  result_.Reset();
  controller_->Start(query, string16(), true, false, true,
      AutocompleteInput::ALL_MATCHES);

  if (!controller_->done())
    // The message loop will terminate when all autocomplete input has been
    // collected.
    MessageLoop::current()->Run();
}

void AutocompleteProviderTest::RunExactKeymatchTest(
    bool allow_exact_keyword_match) {
  // Send the controller input which exactly matches the keyword provider we
  // created in ResetControllerWithKeywordAndSearchProviders().  The default
  // match should thus be a keyword match iff |allow_exact_keyword_match| is
  // true.
  controller_->Start(ASCIIToUTF16("k test"), string16(), true, false,
                     allow_exact_keyword_match,
                     AutocompleteInput::SYNCHRONOUS_MATCHES);
  EXPECT_TRUE(controller_->done());
  // ResetControllerWithKeywordAndSearchProviders() adds the keyword provider
  // first, then the search provider.  So if the default match is a keyword
  // match, it will come from provider 0, otherwise from provider 1.
  EXPECT_EQ(providers_[allow_exact_keyword_match ? 0 : 1],
      controller_->result().default_match()->provider);
}

void AutocompleteProviderTest::Observe(
    int type,
    const content::NotificationSource& source,
    const content::NotificationDetails& details) {
  if (controller_->done()) {
    result_.CopyFrom(controller_->result());
    MessageLoop::current()->Quit();
  }
}

// Tests that the default selection is set properly when updating results.
TEST_F(AutocompleteProviderTest, Query) {
  ResetControllerWithTestProviders(false);
  RunTest();

  // Make sure the default match gets set to the highest relevance match.  The
  // highest relevance matches should come from the second provider.
  EXPECT_EQ(kResultsPerProvider * 2, result_.size());  // two providers
  ASSERT_NE(result_.end(), result_.default_match());
  EXPECT_EQ(providers_[1], result_.default_match()->provider);
}

// Tests assisted query stats.
TEST_F(AutocompleteProviderTest, AssistedQueryStats) {
  ResetControllerWithTestProviders(false);
  RunTest();

  EXPECT_EQ(kResultsPerProvider * 2, result_.size());  // two providers

  // Now, check the results from the second provider, as they should not have
  // assisted query stats set.
  for (size_t i = 0; i < kResultsPerProvider; ++i) {
    EXPECT_TRUE(
        result_.match_at(i)->search_terms_args->assisted_query_stats.empty());
  }
  // The first provider has a test keyword, so AQS should be non-empty.
  for (size_t i = kResultsPerProvider; i < kResultsPerProvider * 2; ++i) {
    EXPECT_FALSE(
        result_.match_at(i)->search_terms_args->assisted_query_stats.empty());
  }
}

TEST_F(AutocompleteProviderTest, RemoveDuplicates) {
  ResetControllerWithTestProviders(true);
  RunTest();

  // Make sure all the first provider's results were eliminated by the second
  // provider's.
  EXPECT_EQ(kResultsPerProvider, result_.size());
  for (AutocompleteResult::const_iterator i(result_.begin());
       i != result_.end(); ++i)
    EXPECT_EQ(providers_[1], i->provider);
}

TEST_F(AutocompleteProviderTest, AllowExactKeywordMatch) {
  ResetControllerWithTestProvidersWithKeywordAndSearchProviders();
  RunExactKeymatchTest(true);
  RunExactKeymatchTest(false);
}

// Test that redundant associated keywords are removed.
TEST_F(AutocompleteProviderTest, RedundantKeywordsIgnoredInResult) {
  ResetControllerWithKeywordProvider();

  // Get the controller's internal members in the correct state.
  RunQuery(ASCIIToUTF16("fo"));

  {
    KeywordTestData duplicate_url[] = {
      { ASCIIToUTF16("fo"), string16(), false },
      { ASCIIToUTF16("foo.com"), string16(), true },
      { ASCIIToUTF16("foo.com"), string16(), false }
    };

    SCOPED_TRACE("Duplicate url");
    RunRedundantKeywordTest(duplicate_url, ARRAYSIZE_UNSAFE(duplicate_url));
  }

  {
    KeywordTestData keyword_match[] = {
      { ASCIIToUTF16("foo.com"), ASCIIToUTF16("foo.com"), false },
      { ASCIIToUTF16("foo.com"), string16(), false }
    };

    SCOPED_TRACE("Duplicate url with keyword match");
    RunRedundantKeywordTest(keyword_match, ARRAYSIZE_UNSAFE(keyword_match));
  }

  {
    KeywordTestData multiple_keyword[] = {
      { ASCIIToUTF16("fo"), string16(), false },
      { ASCIIToUTF16("foo.com"), string16(), true },
      { ASCIIToUTF16("foo.com"), string16(), false },
      { ASCIIToUTF16("bar.com"), string16(), true },
    };

    SCOPED_TRACE("Duplicate url with multiple keywords");
    RunRedundantKeywordTest(multiple_keyword,
        ARRAYSIZE_UNSAFE(multiple_keyword));
  }
}

TEST_F(AutocompleteProviderTest, UpdateAssistedQueryStats) {
  ResetControllerWithTestProviders(false);

  {
    AssistedQueryStatsTestData test_data[] = {
      //  MSVC doesn't support zero-length arrays, so supply some dummy data.
      { AutocompleteMatch::SEARCH_WHAT_YOU_TYPED, "" }
    };
    SCOPED_TRACE("No matches");
    // Note: We pass 0 here to ignore the dummy data above.
    RunAssistedQueryStatsTest(test_data, 0);
  }

  {
    AssistedQueryStatsTestData test_data[] = {
      { AutocompleteMatch::SEARCH_WHAT_YOU_TYPED, "chrome.0.57" }
    };
    SCOPED_TRACE("One match");
    RunAssistedQueryStatsTest(test_data, ARRAYSIZE_UNSAFE(test_data));
  }

  {
    AssistedQueryStatsTestData test_data[] = {
      { AutocompleteMatch::SEARCH_WHAT_YOU_TYPED, "chrome.0.57j58j5l2j0l3j59" },
      { AutocompleteMatch::URL_WHAT_YOU_TYPED, "chrome.1.57j58j5l2j0l3j59" },
      { AutocompleteMatch::NAVSUGGEST, "chrome.2.57j58j5l2j0l3j59" },
      { AutocompleteMatch::NAVSUGGEST, "chrome.3.57j58j5l2j0l3j59" },
      { AutocompleteMatch::SEARCH_SUGGEST, "chrome.4.57j58j5l2j0l3j59" },
      { AutocompleteMatch::SEARCH_SUGGEST, "chrome.5.57j58j5l2j0l3j59" },
      { AutocompleteMatch::SEARCH_SUGGEST, "chrome.6.57j58j5l2j0l3j59" },
      { AutocompleteMatch::SEARCH_HISTORY, "chrome.7.57j58j5l2j0l3j59" },
    };
    SCOPED_TRACE("Multiple matches");
    RunAssistedQueryStatsTest(test_data, ARRAYSIZE_UNSAFE(test_data));
  }
}

typedef testing::Test AutocompleteTest;

TEST_F(AutocompleteTest, InputType) {
  struct test_data {
    const string16 input;
    const AutocompleteInput::Type type;
  } input_cases[] = {
    { string16(), AutocompleteInput::INVALID },
    { ASCIIToUTF16("?"), AutocompleteInput::FORCED_QUERY },
    { ASCIIToUTF16("?foo"), AutocompleteInput::FORCED_QUERY },
    { ASCIIToUTF16("?foo bar"), AutocompleteInput::FORCED_QUERY },
    { ASCIIToUTF16("?http://foo.com/bar"), AutocompleteInput::FORCED_QUERY },
    { ASCIIToUTF16("foo"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("localhost"), AutocompleteInput::URL },
    { ASCIIToUTF16("foo.c"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("foo.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("-foo.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("foo-.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("foo_.com"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("foo.-com"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("foo/"), AutocompleteInput::URL },
    { ASCIIToUTF16("foo/bar"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("foo/bar/"), AutocompleteInput::URL },
    { ASCIIToUTF16("foo/bar baz\\"), AutocompleteInput::URL },
    { ASCIIToUTF16("foo.com/bar"), AutocompleteInput::URL },
    { ASCIIToUTF16("foo;bar"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("foo/bar baz"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("foo bar.com"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("foo bar"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("foo+bar"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("foo+bar.com"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("\"foo:bar\""), AutocompleteInput::QUERY },
    { ASCIIToUTF16("link:foo.com"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("foo:81"), AutocompleteInput::URL },
    { ASCIIToUTF16("localhost:8080"), AutocompleteInput::URL },
    { ASCIIToUTF16("www.foo.com:81"), AutocompleteInput::URL },
    { ASCIIToUTF16("foo.com:123456"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("foo.com:abc"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("1.2.3.4:abc"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("user@foo.com"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("user@foo/z"), AutocompleteInput::URL },
    { ASCIIToUTF16("user@foo/z z"), AutocompleteInput::URL },
    { ASCIIToUTF16("user@foo.com/z"), AutocompleteInput::URL },
    { ASCIIToUTF16("user:pass@"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("user:pass@!foo.com"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("user:pass@foo"), AutocompleteInput::URL },
    { ASCIIToUTF16("user:pass@foo.c"), AutocompleteInput::URL },
    { ASCIIToUTF16("user:pass@foo.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("user:pass@foo.com:81"), AutocompleteInput::URL },
    { ASCIIToUTF16("user:pass@foo:81"), AutocompleteInput::URL },
    { ASCIIToUTF16("1.2"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("1.2/45"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("1.2:45"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("user@1.2:45"), AutocompleteInput::URL },
    { ASCIIToUTF16("user@foo:45"), AutocompleteInput::URL },
    { ASCIIToUTF16("user:pass@1.2:45"), AutocompleteInput::URL },
    { ASCIIToUTF16("host?query"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("host#ref"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("host/path?query"), AutocompleteInput::URL },
    { ASCIIToUTF16("host/path#ref"), AutocompleteInput::URL },
    { ASCIIToUTF16("en.wikipedia.org/wiki/Jim Beam"), AutocompleteInput::URL },
    // In Chrome itself, mailto: will get handled by ShellExecute, but in
    // unittest mode, we don't have the data loaded in the external protocol
    // handler to know this.
    // { ASCIIToUTF16("mailto:abuse@foo.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("view-source:http://www.foo.com/"), AutocompleteInput::URL },
    { ASCIIToUTF16("javascript:alert(\"Hi there\");"), AutocompleteInput::URL },
#if defined(OS_WIN)
    { ASCIIToUTF16("C:\\Program Files"), AutocompleteInput::URL },
    { ASCIIToUTF16("\\\\Server\\Folder\\File"), AutocompleteInput::URL },
#endif  // defined(OS_WIN)
    { ASCIIToUTF16("http:foo"), AutocompleteInput::URL },
    { ASCIIToUTF16("http://foo"), AutocompleteInput::URL },
    { ASCIIToUTF16("http://foo.c"), AutocompleteInput::URL },
    { ASCIIToUTF16("http://foo.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("http://foo_bar.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("http://foo/bar baz"), AutocompleteInput::URL },
    { ASCIIToUTF16("http://-foo.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("http://foo-.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("http://foo_.com"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("http://foo.-com"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("http://_foo_.com"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("http://foo.com:abc"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("http://foo.com:123456"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("http://1.2.3.4:abc"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("http:user@foo.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("http://user@foo.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("http:user:pass@foo.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("http://user:pass@foo.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("http://1.2"), AutocompleteInput::URL },
    { ASCIIToUTF16("http://1.2/45"), AutocompleteInput::URL },
    { ASCIIToUTF16("http:ps/2 games"), AutocompleteInput::URL },
    { ASCIIToUTF16("https://foo.com"), AutocompleteInput::URL },
    { ASCIIToUTF16("127.0.0.1"), AutocompleteInput::URL },
    { ASCIIToUTF16("127.0.1"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("127.0.1/"), AutocompleteInput::URL },
    { ASCIIToUTF16("browser.tabs.closeButtons"), AutocompleteInput::UNKNOWN },
    { WideToUTF16(L"\u6d4b\u8bd5"), AutocompleteInput::UNKNOWN },
    { ASCIIToUTF16("[2001:]"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("[2001:dB8::1]"), AutocompleteInput::URL },
    { ASCIIToUTF16("192.168.0.256"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("[foo.com]"), AutocompleteInput::QUERY },
    { ASCIIToUTF16("filesystem:http://a.com/t/bar"), AutocompleteInput::URL },
    { ASCIIToUTF16("filesystem:http:foo"), AutocompleteInput::URL },
    { ASCIIToUTF16("filesystem:file://"), AutocompleteInput::URL },
    { ASCIIToUTF16("filesystem:http"), AutocompleteInput::URL },
    { ASCIIToUTF16("filesystem:"), AutocompleteInput::URL },
    { ASCIIToUTF16("ftp:"), AutocompleteInput::URL },
  };

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(input_cases); ++i) {
    SCOPED_TRACE(input_cases[i].input);
    AutocompleteInput input(input_cases[i].input, string16(), true, false,
                            true, AutocompleteInput::ALL_MATCHES);
    EXPECT_EQ(input_cases[i].type, input.type());
  }
}

TEST_F(AutocompleteTest, InputTypeWithDesiredTLD) {
  struct test_data {
    const string16 input;
    const AutocompleteInput::Type type;
  } input_cases[] = {
    { ASCIIToUTF16("401k"), AutocompleteInput::REQUESTED_URL },
    { ASCIIToUTF16("999999999999999"), AutocompleteInput::REQUESTED_URL },
    { ASCIIToUTF16("x@y"), AutocompleteInput::REQUESTED_URL },
    { ASCIIToUTF16("y/z z"), AutocompleteInput::REQUESTED_URL },
  };

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(input_cases); ++i) {
    SCOPED_TRACE(input_cases[i].input);
    AutocompleteInput input(input_cases[i].input, ASCIIToUTF16("com"), true,
                            false, true, AutocompleteInput::ALL_MATCHES);
    EXPECT_EQ(input_cases[i].type, input.type());
  }
}

// This tests for a regression where certain input in the omnibox caused us to
// crash. As long as the test completes without crashing, we're fine.
TEST_F(AutocompleteTest, InputCrash) {
  AutocompleteInput input(WideToUTF16(L"\uff65@s"), string16(), true, false,
                          true, AutocompleteInput::ALL_MATCHES);
}

// Test comparing matches relevance.
TEST(AutocompleteMatch, MoreRelevant) {
  struct RelevantCases {
    int r1;
    int r2;
    bool expected_result;
  } cases[] = {
    {  10,   0, true  },
    {  10,  -5, true  },
    {  -5,  10, false },
    {   0,  10, false },
    { -10,  -5, false  },
    {  -5, -10, true },
  };

  AutocompleteMatch m1(NULL, 0, false,
                       AutocompleteMatch::URL_WHAT_YOU_TYPED);
  AutocompleteMatch m2(NULL, 0, false,
                       AutocompleteMatch::URL_WHAT_YOU_TYPED);

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(cases); ++i) {
    m1.relevance = cases[i].r1;
    m2.relevance = cases[i].r2;
    EXPECT_EQ(cases[i].expected_result,
              AutocompleteMatch::MoreRelevant(m1, m2));
  }
}

TEST(AutocompleteInput, ParseForEmphasizeComponent) {
  using url_parse::Component;
  Component kInvalidComponent(0, -1);
  struct test_data {
    const string16 input;
    const Component scheme;
    const Component host;
  } input_cases[] = {
    { string16(), kInvalidComponent, kInvalidComponent },
    { ASCIIToUTF16("?"), kInvalidComponent, kInvalidComponent },
    { ASCIIToUTF16("?http://foo.com/bar"), kInvalidComponent,
        kInvalidComponent },
    { ASCIIToUTF16("foo/bar baz"), kInvalidComponent, Component(0, 3) },
    { ASCIIToUTF16("http://foo/bar baz"), Component(0, 4), Component(7, 3) },
    { ASCIIToUTF16("link:foo.com"), Component(0, 4), kInvalidComponent },
    { ASCIIToUTF16("www.foo.com:81"), kInvalidComponent, Component(0, 11) },
    { WideToUTF16(L"\u6d4b\u8bd5"), kInvalidComponent, Component(0, 2) },
    { ASCIIToUTF16("view-source:http://www.foo.com/"), Component(12, 4),
        Component(19, 11) },
    { ASCIIToUTF16("view-source:https://example.com/"),
      Component(12, 5), Component(20, 11) },
    { ASCIIToUTF16("view-source:www.foo.com"), kInvalidComponent,
        Component(12, 11) },
    { ASCIIToUTF16("view-source:"), Component(0, 11), kInvalidComponent },
    { ASCIIToUTF16("view-source:garbage"), kInvalidComponent,
        Component(12, 7) },
    { ASCIIToUTF16("view-source:http://http://foo"), Component(12, 4),
        Component(19, 4) },
    { ASCIIToUTF16("view-source:view-source:http://example.com/"),
        Component(12, 11), kInvalidComponent }
  };

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(input_cases); ++i) {
    SCOPED_TRACE(input_cases[i].input);
    Component scheme, host;
    AutocompleteInput::ParseForEmphasizeComponents(input_cases[i].input,
                                                   string16(),
                                                   &scheme,
                                                   &host);
    AutocompleteInput input(input_cases[i].input, string16(), true, false,
                            true, AutocompleteInput::ALL_MATCHES);
    EXPECT_EQ(input_cases[i].scheme.begin, scheme.begin);
    EXPECT_EQ(input_cases[i].scheme.len, scheme.len);
    EXPECT_EQ(input_cases[i].host.begin, host.begin);
    EXPECT_EQ(input_cases[i].host.len, host.len);
  }
}
