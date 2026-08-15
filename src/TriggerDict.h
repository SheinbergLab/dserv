#ifndef TRIGGERDICT_H
#define TRIGGERDICT_H

#include <unordered_map>
#include <mutex>
#include <algorithm>
#include "MatchDict.h"

#include <vector>

/*
 * TriggerDict
 *
 *   datapoint name -> the scripts to run when it is published.
 *
 *   A name may carry SEVERAL scripts, run in registration order. Single
 *   subscriber was the surprising choice for a pub/sub system: it meant two
 *   independent consumers of one datapoint in the same interp could not
 *   coexist, so one had to know about the other and call it -- coupling that
 *   showed up as soon as a second consumer wanted slider/position.
 *
 *   insert() keeps REPLACE semantics on purpose, because dpointSetScript has
 *   always meant "this is now the script" and callers re-register to change
 *   behaviour. Turning that into an append would silently double-fire every
 *   existing registration. append() (dpointAddScript) is the additive form.
 */

class TriggerDict
{
 private:
  std::unordered_map<std::string, std::vector<std::string>> map_;
  std::mutex mutex_;

 public:
  /* Replace whatever is registered for key. dpointSetScript. */
  void insert(std::string key, std::string script)
    {
      std::lock_guard<std::mutex> mlock(mutex_);
      map_[key] = std::vector<std::string>{ script };
    }

  /* Add a script, keeping any already registered. dpointAddScript.
     Duplicates are ignored: re-running an init that registers the same
     handler should not make it fire twice. */
  void append(std::string key, std::string script)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto &v = map_[key];
    if (std::find(v.begin(), v.end(), script) == v.end())
      v.push_back(script);
  }

  void remove(std::string key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    map_.erase (key);
  }

  /* Remove ONE script from key, leaving its siblings. Erases the key when
     the last one goes, so an emptied entry stops shadowing a wildcard.
     Returns true if something was removed. */
  bool remove_script(std::string key, std::string script)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    if (iter == map_.end()) return false;
    auto &v = iter->second;
    auto pos = std::find(v.begin(), v.end(), script);
    if (pos == v.end()) return false;
    v.erase(pos);
    if (v.empty()) map_.erase(iter);
    return true;
  }

  void clear()
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    map_.clear ();
  }

  /* All scripts for key. Returns true if the key is registered AT ALL,
     including with an empty script -- the dispatcher relies on that to keep
     an empty exact entry shadowing a wildcard one. */
  bool find(std::string key, std::vector<std::string> &scripts)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);

    if (iter != map_.end()) {
      scripts = iter->second;
      return true;
    }
    return false;
  }

  /* First script for key, or false. Kept for callers that want a single
     string (dpointGetScript) rather than a list. */
  bool find_first(std::string key, std::string &script)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);

    if (iter != map_.end()) {
      script = iter->second.empty() ? std::string() : iter->second.front();
      return true;
    }
    return false;
  }

  /* Registered keys, for introspection (dpointScripts). Snapshot under lock. */
  std::vector<std::string> keys()
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    std::vector<std::string> out;
    out.reserve(map_.size());
    for (auto const& [ key, value ] : map_) out.push_back(key);
    return out;
  }

  /*
   * find_match()
   *
   *   If datapoint matches a registered wildcard key, return true and set
   *   that key's scripts.
   */
  bool find_match(std::string varname, std::vector<std::string> &scripts)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    for (auto const& [ key, value ] : map_) {
      if (MatchDict::FastWildCompare((char *) key.c_str(), (char *) varname.c_str())) {
	scripts = value;
	return true;
      }
    }
    return false;
  }

};

#endif
