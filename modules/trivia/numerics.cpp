/************************************************************************************
 * 
 * TriviaBot, The trivia bot for discord based on Fruitloopy Trivia for ChatSpike IRC
 *
 * Copyright 2004 Craig Edwards <support@brainbox.cc>
 *
 * Core based on Sporks, the Learning Discord Bot, Craig Edwards (c) 2019.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ************************************************************************************/

#include <string>
#include <cstdint>
#include <fstream>
#include <streambuf>
#include <sporks/stringops.h>
#include "trivia.h"

int TriviaModule::random(int min, int max)
{
	return min + rand() % (( max + 1 ) - min);
}

std::string TriviaModule::dec_to_roman(unsigned int decimal, const guild_settings_t &settings)
{
	std::vector<int> numbers =  { 1, 4 ,5, 9, 10, 40, 50, 90, 100, 400, 500, 900, 1000 };
	std::vector<std::string> romans = { "I", "IV", "V", "IX", "X", "XL", "L", "XC", "C", "CD", "D", "CM", "M" };
	std::string result;
	for (int x = 12; x >= 0; x--) {
		while (decimal >= numbers[x]) {
			decimal -= numbers[x];
			result.append(romans[x]);
		}
	}
	return fmt::format(_("ROMAN", settings), result);
}

std::string TriviaModule::tidy_num(std::string num)
{
	std::vector<std::string> param;
	if (number_tidy_dollars->Match(num, param)) {
		num = "$" + ReplaceString(param[1], ",", "");
	}
	if (num.length() > 1 && num[0] == '$') {
		num = ReplaceString(num, ",", "");
	}
	if (number_tidy_nodollars->Match(num, param)) {
		std::string numbers = param[1];
		std::string suffix = param[2];
		numbers = ReplaceString(numbers, ",", "");
		num = numbers + " " + suffix;
	}
	if (number_tidy_positive->Match(num) || number_tidy_negative->Match(num)) {
		num = ReplaceString(num, ",", "");
	}
	return num;
}

std::string TriviaModule::conv_num(std::string datain, const guild_settings_t &settings)
{
	std::map<std::string, int> nn= {
		{ _("ONE", settings), 1 },
		{ _("TWO", settings), 2 },
		{ _("THREE", settings), 3 },
		{ _("FOUR", settings), 4 },
		{ _("FIVE", settings), 5 },
		{ _("SIX", settings), 6 },
		{ _("SEVEN", settings), 7 },
		{ _("EIGHT", settings), 8 },
		{ _("NINE", settings), 9 },
		{ _("TEN", settings), 10 },
		{ _("ELEVEN", settings), 11 },
		{ _("TWELVE", settings), 12 },
		{ _("THIRTEEN", settings), 13 },
		{ _("FOURTEEN", settings), 14 },
		{ _("FIFTEEN", settings), 15 },
		{ _("SIXTEEN", settings), 16 },
		{ _("SEVENTEEN", settings), 17 },
		{ _("EIGHTEEN", settings), 18 },
		{ _("NINETEEN", settings), 19 },
		{ _("TWENTY", settings), 20 },
		{ _("THIRTY", settings), 30 },
		{ _("FOURTY", settings), 40 },
		{ _("FIFTY", settings), 50 },
		{ _("SIXTY", settings), 60 },
		{ _("SEVENTY", settings), 70 },
		{ _("EIGHTY", settings), 80 },
		{ _("NINETY", settings), 90 }
	};
	if (datain.empty()) {
		datain = _("ZERO", settings);
	}
	datain = ReplaceString(datain, "  ", " ");
	datain = ReplaceString(datain, "-", "");
	datain = ReplaceString(datain, _("AND_SPACED", settings), " ");
	int last = 0;
	int initial = 0;
	std::string currency;
	std::vector<std::string> nums;
	std::stringstream str(datain);
	std::string v;
	while ((str >> v)) {
		nums.push_back(v);
	}
	for (auto x = nums.begin(); x != nums.end(); ++x) {
		if (nn.find(lowercase(*x)) == nn.end() && !PCRE(_("MILLION", settings), true).Match(*x) && !PCRE(_("THOUSAND", settings), true).Match(*x) && !PCRE(_("HUNDRED", settings), true).Match(*x) && !PCRE(_("DOLLARS", settings), true).Match(*x)) {
			return "0";
		}
	}
	for (auto next = nums.begin(); next != nums.end(); ++next) {
		std::string nextnum = lowercase(*next);
		auto ahead = next;
		ahead++;
		std::string lookahead = "";
		if (ahead != nums.end()) {
			lookahead = *ahead;
		}
		if (nn.find(nextnum) != nn.end()) {
			last = nn.find(nextnum)->second;
		}
		if (PCRE(_("DOLLARS", settings), true).Match(nextnum)) {
			currency = "$";
			last = 0;
		}
		if (!PCRE(_("HTM_REGEX", settings), true).Match(lookahead)) {
			initial += last;
			last = 0;
		} else {
			if (PCRE(_("HUNDRED", settings), true).Match(lookahead)) {
				initial += last * 100;
				last = 0;
			} else if (PCRE(_("THOUSAND", settings), true).Match(lookahead)) {
				initial += last * 1000;
				last = 0;
			} else if (PCRE(_("MILLION", settings), true).Match(lookahead)) {
				initial += last * 1000000;
				last = 0;
			}
		}
	}
	return currency + std::to_string(initial);
}

std::string TriviaModule::numbertoname(int64_t number, const guild_settings_t& settings)
{

	for (auto x = numstrs.begin(); x != numstrs.end(); ++x) {
		uint64_t value = (*x)["value"].get<uint64_t>();
		if (value == number) {
			if (settings.language == "en")
				return (*x)["description"].get<std::string>();
			else
				return (*x)[fmt::format("trans_{}", settings.language)].get<std::string>();
		}
	}
	return std::to_string(number);
}

std::string TriviaModule::GetNearestNumber(int64_t number, const guild_settings_t& settings)
{
	for (auto x = numstrs.begin(); x != numstrs.end(); ++x) {
		uint64_t value = (*x)["value"].get<uint64_t>();
		if (value <= number) {
			if (settings.language == "en")
				return (*x)["description"].get<std::string>();
			else
				return (*x)[fmt::format("trans_{}", settings.language)].get<std::string>();
		}
	}
	return "0";
}

int64_t TriviaModule::GetNearestNumberVal(int64_t number, const guild_settings_t& settings)
{
	for (auto x = numstrs.begin(); x != numstrs.end(); ++x) {
		uint64_t value = (*x)["value"].get<uint64_t>();
		if (value <= number) {
			return value;
		}
	}
	return 0;
}

bool TriviaModule::is_number(const std::string &s)
{
	return !s.empty() && std::find_if(s.begin(), s.end(), [](unsigned char c) { return !std::isdigit(c); }) == s.end();
}

std::string TriviaModule::MakeFirstHint(const std::string &s, const guild_settings_t &settings, bool indollars)
{
	std::string Q;
	if (is_number(s)) {
		std::string plus = _("COMMA_PLUS_SPACE", settings);
		int64_t n = from_string<int64_t>(s, std::dec);
		while (GetNearestNumberVal(n, settings) != 0 && n > 0) {
			Q.append(GetNearestNumber(n, settings)).append(plus);
			n -= GetNearestNumberVal(n, settings);
		}
		if (n > 0) {
			Q.append(numbertoname(n, settings));
		}
		Q = Q.substr(0, Q.length() - plus.length());
	}
	if (Q.empty()) {
		return _("LOWEST_NONNEG", settings);
	}
	if (indollars) {
		return fmt::format(_("INDOLLARS", settings), Q);
	} else {
		return Q;
	}
}

