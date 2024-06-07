#ifndef MATCHDICT_H
#define MATCHDICT_H

#include <unordered_map>
#include <mutex>

class MatchSpec
{
 public:
  enum match_type {
    MATCH_EXACT, MATCH_BEGIN, MATCH_END, MATCH_ANYWHERE, MATCH_KRAUSS,
  }; 
  
 public:
  int active;
  int alert_every;
  int count;
  match_type type;
  std::string matchstr;

  MatchSpec(void) {};
  MatchSpec(const char *str, int every=1)
    {
      active = 1;
      alert_every = every;
      count = 0;
      type = MATCH_KRAUSS;
      matchstr = std::string(str);
    };
  MatchSpec(const char *str, match_type type, int every=1): type(type)
    {
      active = 1;
      alert_every = every;
      count = 0;
      matchstr = std::string(str);
    };
}; 

class MatchDict
{
 private:
  std::unordered_map<std::string, MatchSpec> map_;
  std::mutex mutex_;

 public:
  void insert(std::string key, MatchSpec m)
    {
      std::lock_guard<std::mutex> mlock(mutex_);
      map_[key] = m;
    }

  void remove(std::string key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    map_.erase (key);
  }
  
  void clear(void)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    map_.clear ();
  }
  
  bool find(std::string key, MatchSpec *m)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    
    if (iter != map_.end()) {
      if (m) *m = iter->second;
      return true;
    }
    return false;
  }

  std::string to_string(void) {
    std::string s("{ ");

    for (auto it : map_) {
      s += it.first;
      s += " ";
    }
    s += "}";
    return s;
  }
  
  // Copyright 2018 IBM Corporation
  // 
  // Licensed under the Apache License, Version 2.0 (the "License");
  // you may not use this file except in compliance with the License.
  // You may obtain a copy of the License at
  // 
  //     http://www.apache.org/licenses/LICENSE-2.0
  // 
  // Unless required by applicable law or agreed to in writing, software
  // distributed under the License is distributed on an "AS IS" BASIS,
  // WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  // See the License for the specific language governing permissions and
  // limitations under the License.
  //
  // Compares two text strings.  Accepts '?' as a single-character wildcard.  
  // For each '*' wildcard, seeks out a matching sequence of any characters 
  // beyond it.  Otherwise compares the strings a character at a time. 
  //
  static bool FastWildCompare(char *pWild, char *pTame)
  {
    char *pWildSequence;  // Points to prospective wild string match after '*'
    char *pTameSequence;  // Points to prospective tame string match
  
    // Find a first wildcard, if one exists, and the beginning of any  
    // prospectively matching sequence after it.
    do
      {
	// Check for the end from the start.  Get out fast, if possible.
	if (!*pTame)
	  {
	    if (*pWild)
	      {
		while (*(pWild++) == '*')
		  {
		    if (!(*pWild))
		      {
			return true;   // "ab" matches "ab*".
		      }
		  }
	      
		return false;          // "abcd" doesn't match "abc".
	      }
	    else
	      {
		return true;           // "abc" matches "abc".
	      }
	  }
	else if (*pWild == '*')
	  {
	    // Got wild: set up for the second loop and skip on down there.
	    while (*(++pWild) == '*')
	      {
		continue;
	      }
	  
	    if (!*pWild)
	      {
		return true;           // "abc*" matches "abcd".
	      }
	  
	    // Search for the next prospective match.
	    if (*pWild != '?')
	      {
		while (*pWild != *pTame)
		  {
		    if (!*(++pTame))
		      {
			return false;  // "a*bc" doesn't match "ab".
		      }
		  }
	      }
	  
	    // Keep fallback positions for retry in case of incomplete match.
	    pWildSequence = pWild;
	    pTameSequence = pTame;
	    break;
	  }
	else if (*pWild != *pTame && *pWild != '?')
	  {
	    return false;              // "abc" doesn't match "abd".
	  }
      
	++pWild;                       // Everything's a match, so far.
	++pTame;
      } while (true);
  
    // Find any further wildcards and any further matching sequences.
    do
      {
	if (*pWild == '*')
	  {
	    // Got wild again.
	    while (*(++pWild) == '*')
	      {
		continue;
	      }
	  
	    if (!*pWild)
	      {
		return true;           // "ab*c*" matches "abcd".
	      }
	  
	    if (!*pTame)
	      {
		return false;          // "*bcd*" doesn't match "abc".
	      }
	  
	    // Search for the next prospective match.
	    if (*pWild != '?')
	      {
		while (*pWild != *pTame)
		  {
		    if (!*(++pTame))
		      {
			return false;  // "a*b*c" doesn't match "ab".
		      }
		  }
	      }
	  
	    // Keep the new fallback positions.
	    pWildSequence = pWild;
	    pTameSequence = pTame;
	  }
	else if (*pWild != *pTame && *pWild != '?')
	  {
	    // The equivalent portion of the upper loop is really simple.
	    if (!*pTame)
	      {
		return false;          // "*bcd" doesn't match "abc".
	      }
	  
	    // A fine time for questions.
	    while (*pWildSequence == '?')
	      {
		++pWildSequence;
		++pTameSequence;
	      }
	  
	    pWild = pWildSequence;
	  
	    // Fall back, but never so far again.
	    while (*pWild != *(++pTameSequence))
	      {
		if (!*pTameSequence)
		  {
		    return false;      // "*a*b" doesn't match "ac".
		  }
	      }
	  
	    pTame = pTameSequence;
	  }
      
	// Another check for the end, at the end.
	if (!*pTame)
	  {
	    if (!*pWild)
	      {
		return true;           // "*bc" matches "abc".
	      }
	    else
	      {
		return false;          // "*bc" doesn't match "abcd".
	      }
	  }
      
	++pWild;                       // Everything's still a match.
	++pTame;
      } while (true);
  }

  // check all match specs for a match
  // need to check all of them to ensure alert_every counter is updated
  bool is_match(char *var)
  {
    bool ret = false;
    for (auto&& it : map_) {
      MatchSpec *match = &it.second;
      if (!match->active) continue;
	
      switch (match->type) {
      case MatchSpec::MATCH_EXACT:
	if (!strcmp(var, match->matchstr.c_str()) &&
	    !(match->count++ % match->alert_every))
	  ret = true;
	break;
      case MatchSpec::MATCH_KRAUSS:
	if (FastWildCompare((char *) match->matchstr.c_str(), var) &&
	    !(match->count++ % match->alert_every))
	  ret = true;
	break;
      case MatchSpec::MATCH_BEGIN:
      case MatchSpec::MATCH_END:
      case MatchSpec::MATCH_ANYWHERE:
	break;
      }
    }
    return ret;
  }
};


#endif

