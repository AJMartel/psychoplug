/*
  PsychoPlug
  ESP8266 based remote outlet with standalone timer and MQTT integration
  
  Copyright (C) 2017  Earle F. Philhower, III

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <Arduino.h>
#include <TimeLib.h>
#include "timezone.h"
#include "log.h"

#include "tz.h"

typedef struct tm {
  uint8_t tm_sec; 
  uint8_t tm_min; 
  uint8_t tm_hour; 
  uint8_t tm_wday;
  uint8_t tm_mday;
  uint8_t tm_mon; 
  uint16_t tm_year;
  uint8_t tm_isdst;
} tm;


time_t mktime(struct tm *tm)
{
  tmElements_t tme;
  tme.Second = tm->tm_sec;
  tme.Minute = tm->tm_min; 
  tme.Hour = tm->tm_hour; 
  tme.Wday = tm->tm_wday+1;   // day of week, sunday is day 1
  tme.Day = tm->tm_mday;
  tme.Month = tm->tm_mon; 
  tme.Year = (uint8_t)(tm->tm_year - 70); // offset from 1970; 
  time_t makeTime(tmElements_t &tm); // convert time elements into time_t
}

struct tm *gmtime(const time_t *timep)
{
  static struct tm tm;
  tmElements_t tme;
  breakTime(*timep, tme); // break time_t into elements
  tm.tm_sec = tme.Second;
  tm.tm_min = tme.Minute;
  tm.tm_hour = tme.Hour;
  tm.tm_wday = tme.Wday-1; // Sun=0 in tm, 1 in tme
  tm.tm_mday = tme.Day;
  tm.tm_mon = tme.Month;
  tm.tm_year = tme.Year + 70; // 0=1900 in tm, 1970 in tme
  return &tm;
}

char *GetNextTZ(bool reset, char *buff, char buffLen)
{
	static uint16_t idxTZ = 0;
	static uint16_t idxLink = 0;
	if (reset) {
		idxLink = 0;
		idxTZ = 0;
	}

  const int tzcount = sizeof(mtimezone)/sizeof(mtimezone[0]);
  const int linkcount = sizeof(link)/sizeof(link[0]);
	if (idxTZ < tzcount) {
		strlcpy(buff + mtimezone[idxTZ].zoneNameFromPrev,  mtimezone[idxTZ].zonename, buffLen - mtimezone[idxTZ].zoneNameFromPrev);
		idxTZ++;
		return buff;
	} else if (idxLink < linkcount) {
		strlcpy(buff + link[idxLink].zoneNameFromPrev,  link[idxLink].zonename, buffLen - link[idxLink].zoneNameFromPrev);
		idxLink++;
		return buff;
	} else {
		return NULL;
	}
}

int FindTZName(const char *tzname)
{
	char buff[64];
  const int tzcount = sizeof(mtimezone)/sizeof(mtimezone[0]);
  const int linkcount = sizeof(link)/sizeof(link[0]);
	for (int i=0; i < tzcount; i++) {
		strlcpy(buff + mtimezone[i].zoneNameFromPrev,  mtimezone[i].zonename, sizeof(buff) - mtimezone[i].zoneNameFromPrev);
		if (!strcmp(buff, tzname)) return i;
	}
	for (int i=0; i < linkcount; i++) {
		strlcpy(buff + link[i].zoneNameFromPrev,  link[i].zonename, sizeof(buff) - link[i].zoneNameFromPrev);
		if (!strcmp(buff, tzname)) return link[i].timezone;
	}
	return -1;
}

static uint16_t timezoneNum = 0; // Which timezone id are we presently handling?
static time_t utcOffsetSecs = 0; // Unadjusted UTC offset
static bool useDSTRule = false;  // Does the current TZ need DST handling?
static uint16_t dstYear = 1900;  // What year the cached computations below are falid for
static time_t dstChangeAtUTC[2]; // UTC time when the offset below takes effect
static time_t dstOffsetSecs[2];  // Delta from default UTC offset to apply
static char timezoneStr[16];     // Human readable string w/format specifiers for DST/non-DST
static char dstString[2][4];     // Format specifier replacement

void UpdateDSTInfo(time_t whenUTC)
{
  const int ruleCount = sizeof(rules)/sizeof(rules[0]);
	rule_t dstRule[2];
	struct tm t;
	int ruleIdx;

  Log("Updating DST Info");

	ruleIdx = 0;
	for (int i=0; i<ruleCount && ruleIdx < 2; i++) {
		if (rules[i].name == mtimezone[timezoneNum].rule) {
			memcpy(dstRule+ruleIdx, &rules[i], sizeof(rules[i]));
			ruleIdx++;
		}
	}
	if (ruleIdx != 2) {
		// Something weird, we don't understand if there's only one
		useDSTRule = false;
		return;
	}

  strlcpy(dstString[0], dstRule[0].fmtstr, sizeof(dstString[0]));
  strlcpy(dstString[1], dstRule[1].fmtstr, sizeof(dstString[1]));

	memcpy(&t, gmtime(&whenUTC), sizeof(t));
	int curYear = 1900 + t.tm_year;
	dstYear = curYear;

	// Calculate the time when each rule will fire this year
	for (int i=0; i<2; i++) {
		memset(&t, sizeof(t), 0);
		t.tm_year = curYear - 1900;
		t.tm_mon = dstRule[i].month;
		t.tm_mday = 1;
		t.tm_isdst = 0;
		t.tm_hour = 0;
		t.tm_min = 0;
		t.tm_sec = 0;
		time_t at = mktime(&t);
		// Fill in the wday, yday field
		memcpy(&t, gmtime(&at), sizeof(t));
		// Now we have "t" which has the UTC time for day 1 of the month.
		// Adjust to the proper day mechanically.  Could be optimized
		switch (dstRule[i].daytrig) {
			case SUN_GTEQ:
			case SUN_LAST:
				while (0!=t.tm_wday) {t.tm_wday = (t.tm_wday+1)%7; t.tm_mday++; at += SECS_PER_DAY; }
				break;
			case MON_GTEQ:
			case MON_LAST:
				while (1!=t.tm_wday) {t.tm_wday = (t.tm_wday+1)%7; t.tm_mday++; at += SECS_PER_DAY; }
				break;
			case TUE_GTEQ:
			case TUE_LAST:
				while (2!=t.tm_wday) {t.tm_wday = (t.tm_wday+1)%7; t.tm_mday++; at += SECS_PER_DAY; }
				break;
			case WED_GTEQ:
			case WED_LAST:
				while (3!=t.tm_wday) {t.tm_wday = (t.tm_wday+1)%7; t.tm_mday++; at += SECS_PER_DAY; }
				break;
			case THU_GTEQ:
			case THU_LAST:
				while (4!=t.tm_wday) {t.tm_wday = (t.tm_wday+1)%7; t.tm_mday++; at += SECS_PER_DAY; }
				break;
			case FRI_GTEQ:
			case FRI_LAST:
				while (5!=t.tm_wday) {t.tm_wday = (t.tm_wday+1)%7; t.tm_mday++; at += SECS_PER_DAY; }
				break;
			case SAT_GTEQ:
			case SAT_LAST:
				while (6!=t.tm_wday) {t.tm_wday = (t.tm_wday+1)%7; t.tm_mday++; at += SECS_PER_DAY; }
				break;

			case GIVEN_DAY:  // Simplest, update day and recalc AT
				t.tm_mday = dstRule[i].daynum;
				at = mktime(&t);
				break;
		}
		switch (dstRule[i].daytrig) {
			case SUN_GTEQ:
			case MON_GTEQ:
			case TUE_GTEQ:
			case WED_GTEQ:
			case THU_GTEQ:
			case FRI_GTEQ:
			case SAT_GTEQ:
				while (t.tm_mday < dstRule[i].daynum) { t.tm_mday += 7; at += SECS_PER_DAY * 7; }
				break;
			case SUN_LAST:
			case MON_LAST:
			case TUE_LAST:
			case WED_LAST:
			case THU_LAST:
			case FRI_LAST:
			case SAT_LAST:
				// Go to the next month and work backwards
				while (t.tm_mon == dstRule[i].month) {
					at += 7 * SECS_PER_DAY;
					memcpy(&t, gmtime(&at), sizeof(t));
				}
				at -= 7 * SECS_PER_DAY; // Back one week
				memcpy(&t, gmtime(&at), sizeof(t));
				break;
		}
		// Now at == t == day of the event
		int secToFire = abs(dstRule[i].athr) * SECS_PER_HOUR + dstRule[i].atmin * SECS_PER_MIN;
		if (dstRule[i].athr<0) secToFire = -secToFire;
		at += secToFire; 
		
		// Now see where this time is relative to and adjust appropriately.
		int dstoffsecs = abs(dstRule[i].offsethrs) * 3600 + dstRule[i].offsetmins;
		if (dstRule[i].offsethrs<0) dstoffsecs = -dstoffsecs;
		int otherdstoffsecs = abs(dstRule[i?0:1].offsethrs) * 3600 + dstRule[i?0:1].offsetmins;
		if (dstRule[i?0:1].offsethrs<0) otherdstoffsecs = -otherdstoffsecs;
		switch(dstRule[i].atref) {
			case ATREF_U:
				// Relative to UTC, so no change needed
				break;
			case ATREF_S:
				// Relative to unadjusted local time (no DST jiggery)
				at -= utcOffsetSecs;
				break;
			case ATREF_W:
				// Relative to wall time (local adjusted by DST)...by the *other* rule!
				at -= utcOffsetSecs;
				at -= otherdstoffsecs;
				break;
		}
		memcpy(&t, gmtime(&at), sizeof(t));
		dstChangeAtUTC[i] = at;
		dstOffsetSecs[i] = dstoffsecs;
	}
}

time_t LocalTime(time_t whenUTC)
{
	struct tm t;
  memcpy(&t, gmtime(&whenUTC), sizeof(t));
	if ((dstYear != t.tm_year + 1900) && useDSTRule) UpdateDSTInfo(whenUTC);
	if (!useDSTRule) {
		// Just UTC offset needed 
		return whenUTC + utcOffsetSecs;
	} else if ((whenUTC >= dstChangeAtUTC[0]) && (whenUTC < dstChangeAtUTC[1])) {
		// At 1st DST change
		return whenUTC + utcOffsetSecs + dstOffsetSecs[0];
	} else {
		// After 2nd change or before 1st
		return whenUTC + utcOffsetSecs + dstOffsetSecs[1];
	}
}

char *AscTime(time_t whenUTC, bool use12Hr, bool useDMY, char *buff, int buffLen)
{
  time_t t = LocalTime(whenUTC);

  char tzID[16];
  if (!useDSTRule) {
    // No TZ string replacement needed
    strlcpy(tzID, timezoneStr, sizeof(tzID));
  } else if ((whenUTC >= dstChangeAtUTC[0]) && (whenUTC < dstChangeAtUTC[1])) {
    // At 1st DST change
    snprintf(tzID, sizeof(tzID), timezoneStr, dstString[0]);
  } else {
    // After 2nd change or before 1st
    snprintf(tzID, sizeof(tzID), timezoneStr, dstString[1]);
  }

  int h = hour(t);
  int m = minute(t);
  int s = second(t);
  int mn = month(t);
  int dy = day(t);
  int yr = year(t);

  const char *ampm = "";
  if (use12Hr) {
    if (h==0) { h = 12; ampm = "AM"; }
    else if (h==12) { ampm = "PM"; }
    else if (h<12) { ampm = "AM"; }
    else { h -= 12; ampm = "PM"; }
  }
  snprintf(buff, buffLen, "%d:%02d:%02d%s %s, %d/%d/%d", h, m, s, ampm, tzID, useDMY?dy:mn, useDMY?mn:dy, yr);

  return buff;
}


bool SetTZ(const char *tzName)
{
	timezoneNum = FindTZName(tzName);
	if (timezoneNum<0) timezoneNum = FindTZName("UTC"); // Default to UTC/GMT

	// Store the UTC offset in seconds
	utcOffsetSecs = abs(mtimezone[timezoneNum].gmtoffhr) * 3600 + mtimezone[timezoneNum].gmtoffmin;
	if (mtimezone[timezoneNum].gmtoffhr < 0) utcOffsetSecs = -utcOffsetSecs;

  // Store the timezone human readable string
  strlcpy(timezoneStr, mtimezone[timezoneNum].formatstr, sizeof(timezoneStr));

	if (mtimezone[timezoneNum].rule == RULE_NONE) {
		useDSTRule = false;
	} else {
		// Calculate what UTC time we have to change and store it away
		useDSTRule = true;
		dstYear = 0; // Force it to fix on the 1st call
	}
}

#if 0
void main(int argc, const char *argv[])
{
	setenv("TZ", "", 1);
	if (argc<2) { printf("Please enter a timezone parameter\n"); exit(1); }
	SetTZ(argv[1]);

	time_t now;
	time(&now);
	for (int i =0; i<365; i++) {
		for (int j=0; j<24; j++) {
			for (int k=0; k<60; k++) {
				time_t local = LocalTime(now);
				struct tm t;
				memcpy(&t, gmtime(&local), sizeof(t));
				printf("%d/%02d/%d @ %d:%02d\n", t.tm_mon+1, t.tm_mday, t.tm_year+1900, t.tm_hour, t.tm_min);
				now += SECS_PER_MIN;
			}
		}
	}
}

#endif
