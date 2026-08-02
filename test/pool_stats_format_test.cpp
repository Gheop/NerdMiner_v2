// Host test for the ckpool stats format -- BitMaker-hub/NerdMiner_v2#739
// Runs the old and new filter/parse logic against real payloads from both pool
// families and shows what each produces.
//
//   g++ -O0 -I<path-to-ArduinoJson/src> -o t test/pool_stats_format_test.cpp && ./t
#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// Real payload from pool.nerdminers.org, as posted in issue #739.
static const char CKPOOL[] = R"({
 "hashrate1m": "0", "hashrate5m": "0", "hashrate1hr": "0",
 "hashrate1d": "44.4K", "hashrate7d": "97K",
 "lastshare": 1766172050, "workers": 1, "shares": 25621.0,
 "bestshare": 71.23833398347209, "bestever": 71.23833398347209,
 "authorised": 1765573562,
 "worker": [{ "workername": "bc1qexample.w1",
   "hashrate1m": "0", "hashrate5m": "0", "hashrate1hr": "302K",
   "hashrate1d": "44.4K", "hashrate7d": "97K",
   "lastshare": 1766172050, "shares": 25621.0,
   "bestshare": 71.23833398347209, "bestever": 71.23833398347209 }]
})";

// Equivalent payload shape from public-pool.io.
static const char PUBLICPOOL[] = R"({
 "bestDifficulty": 71.238, "workersCount": 1,
 "workers": [{ "sessionId": "c068be21", "hashRate": 302000 }]
})";

static double parseSuffixedNumber(const char *s) {
  if (!s || !*s) return 0;
  char *end = NULL;
  double v = strtod(s, &end);
  if (!end) return v;
  while (*end == ' ') end++;
  switch (*end) {
    case 'K': case 'k': return v * 1e3;
    case 'M': case 'm': return v * 1e6;
    case 'G': case 'g': return v * 1e9;
    case 'T': case 't': return v * 1e12;
    case 'P': case 'p': return v * 1e15;
    default:            return v;
  }
}

struct Result { int workers; double hashrate; double best; bool best_found; };

static Result parse_original(const char *payload) {
  StaticJsonDocument<300> filter;
  filter["bestDifficulty"] = true;
  filter["workersCount"] = true;
  filter["workers"][0]["sessionId"] = true;
  filter["workers"][0]["hashRate"] = true;
  StaticJsonDocument<2048> doc;
  deserializeJson(doc, payload, DeserializationOption::Filter(filter));

  Result r = {0, 0, 0, false};
  if (doc.containsKey("workersCount")) r.workers = doc["workersCount"].as<int>();
  for (const JsonObject &w : doc["workers"].as<JsonArray>())
    r.hashrate += w["hashRate"].as<double>();
  if (doc.containsKey("bestDifficulty")) { r.best = doc["bestDifficulty"].as<double>(); r.best_found = true; }
  return r;
}

static Result parse_fixed(const char *payload) {
  StaticJsonDocument<400> filter;
  filter["bestDifficulty"] = true;
  filter["workersCount"] = true;
  filter["workers"][0]["sessionId"] = true;
  filter["workers"][0]["hashRate"] = true;
  filter["bestshare"] = true;
  filter["workers"] = true;
  filter["worker"][0]["workername"] = true;
  filter["worker"][0]["hashrate1hr"] = true;
  StaticJsonDocument<2048> doc;
  deserializeJson(doc, payload, DeserializationOption::Filter(filter));

  Result r = {0, 0, 0, false};
  if (doc.containsKey("worker")) {
    r.workers = doc["workers"].as<int>();
    for (const JsonObject &w : doc["worker"].as<JsonArray>())
      r.hashrate += parseSuffixedNumber(w["hashrate1hr"].as<const char *>());
  } else {
    if (doc.containsKey("workersCount")) r.workers = doc["workersCount"].as<int>();
    for (const JsonObject &w : doc["workers"].as<JsonArray>())
      r.hashrate += w["hashRate"].as<double>();
  }
  const char *key = doc.containsKey("bestDifficulty") ? "bestDifficulty"
                  : (doc.containsKey("bestshare") ? "bestshare" : NULL);
  if (key) { r.best = doc[key].as<double>(); r.best_found = true; }
  return r;
}

static void show(const char *name, const char *payload) {
  Result o = parse_original(payload), f = parse_fixed(payload);
  printf("%-14s | %-8s | %-12s | %s\n", name, "workers", "hashrate", "bestDifficulty");
  printf("  original     | %-8d | %-12.0f | %s\n", o.workers, o.hashrate,
         o.best_found ? "found" : "MISSING");
  printf("  fixed        | %-8d | %-12.0f | %s\n\n", f.workers, f.hashrate,
         f.best_found ? "found" : "MISSING");
}

int main() {
  printf("Expected in both cases: 1 worker, 302000 H/s, bestDifficulty found\n\n");
  show("ckpool", CKPOOL);
  show("public-pool", PUBLICPOOL);

  Result c = parse_fixed(CKPOOL), p = parse_fixed(PUBLICPOOL);
  bool ok = c.workers == 1 && c.hashrate == 302000 && c.best_found
         && p.workers == 1 && p.hashrate == 302000 && p.best_found;
  printf("%s\n", ok ? "PASS: both formats parse correctly" : "FAIL");
  return ok ? 0 : 1;
}
