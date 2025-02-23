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

#include <sporks/modules.h>
#include <sporks/regex.h>
#include <string>
#include <cstdint>
#include <fstream>
#include <streambuf>
#include <sporks/stringops.h>
#include <sporks/statusfield.h>
#include <sporks/database.h>
#include "state.h"
#include "trivia.h"
#include "webrequest.h"
#include "piglatin.h"

TriviaModule::TriviaModule(Bot* instigator, ModuleLoader* ml) : Module(instigator, ml), terminating(false)
{
	srand(time(NULL) * time(NULL));
	ml->Attach({ I_OnMessage, I_OnPresenceUpdate, I_OnChannelDelete, I_OnGuildDelete, I_OnAllShardsReady }, this);
	notvowel = new PCRE("/[^aeiou_]/", true);
	number_tidy_dollars = new PCRE("^([\\d\\,]+)\\s+dollars$");
	number_tidy_nodollars = new PCRE("^([\\d\\,]+)\\s+(.+?)$");
	number_tidy_positive = new PCRE("^[\\d\\,]+$");
	number_tidy_negative = new PCRE("^\\-[\\d\\,]+$");
	prefix_match = new PCRE("prefix");
	set_io_context(bot->io, Bot::GetConfig("apikey"));
	presence_update = new std::thread(&TriviaModule::UpdatePresenceLine, this);
	command_processor = new std::thread(&TriviaModule::ProcessCommands, this);
	startup = time(NULL);
	{
		std::lock_guard<std::mutex> cmd_list_lock(cmds_mutex);
		api_commands = get_api_command_names();
		bot->core.log->info("Initial API command count: {}", api_commands.size());
	}
	numstrs = get_num_strs();
	bot->core.log->info("Numstrs count: {}", numstrs.size());

	std::ifstream langfile("../lang.json");
	lang = new json();
	langfile >> *lang;
	bot->core.log->info("Language strings count: {}", lang->size());
	SetupCommands();
}

void TriviaModule::queue_command(const std::string &message, int64_t author, int64_t channel, int64_t guild, bool mention)
{
	std::lock_guard<std::mutex> cmd_lock(cmdmutex);
	commandqueue.push_back(in_cmd(message, author, channel, guild, mention));
}

void TriviaModule::ProcessCommands()
{
	while (!terminating) {
		{
			std::lock_guard<std::mutex> cmd_lock(cmdmutex);
			if (!commandqueue.empty()) {
				to_process.clear();
				for (auto m = commandqueue.begin(); m != commandqueue.end(); ++m) {
					to_process.push_back(*m);
				}
				commandqueue.clear();
			}
		}
		if (!to_process.empty()) {
			for (auto m = to_process.begin(); m != to_process.end(); ++m) {
				handle_command(*m);
			}
			to_process.clear();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

Bot* TriviaModule::GetBot()
{
	return bot;
}

TriviaModule::~TriviaModule()
{
	DisposeThread(presence_update);
	DisposeThread(command_processor);
	std::lock_guard<std::mutex> user_cache_lock(states_mutex);
	states.clear();
	delete notvowel;
	delete number_tidy_dollars;
	delete number_tidy_nodollars;
	delete number_tidy_positive;
	delete number_tidy_negative;
	delete prefix_match;
	delete lang;
}


bool TriviaModule::OnPresenceUpdate()
{
	/* Note: Only updates this cluster's shards! */
	const aegis::shards::shard_mgr& s = bot->core.get_shard_mgr();
	const std::vector<std::unique_ptr<aegis::shards::shard>>& shards = s.get_shards();
	for (auto i = shards.begin(); i != shards.end(); ++i) {
		const aegis::shards::shard* shard = i->get();
		db::query("INSERT INTO infobot_shard_status (id, cluster_id, connected, online, uptime, transfer, transfer_compressed) VALUES('?','?','?','?','?','?','?') ON DUPLICATE KEY UPDATE cluster_id = '?', connected = '?', online = '?', uptime = '?', transfer = '?', transfer_compressed = '?'",
			{
				shard->get_id(),
				bot->GetClusterID(),
				shard->is_connected(),
				shard->is_online(),
				shard->uptime(),
				shard->get_transfer_u(),
				shard->get_transfer(),
				bot->GetClusterID(),
				shard->is_connected(),
				shard->is_online(),
				shard->uptime(),
				shard->get_transfer_u(),
				shard->get_transfer()
			}
		);
	}
	{
		std::lock_guard<std::mutex> cmd_list_lock(cmds_mutex);
		api_commands = get_api_command_names();
	}
	return true;
}

std::string TriviaModule::_(const std::string &k, const guild_settings_t& settings)
{
	auto o = lang->find(k);
	if (o != lang->end()) {
		auto v = o->find(settings.language);
		if (v != o->end()) {
			return v->get<std::string>();
		} else {
			bot->core.log->debug("Missing language '{}' in string '{}'!", settings.language, k);
		}
	} else {
		bot->core.log->debug("Missing language string '{}'", k);
	}
	return k;
}

bool TriviaModule::OnAllShardsReady()
{
	char hostname[1024];
	hostname[1023] = '\0';
	gethostname(hostname, 1023);
	json active = get_active(hostname, bot->GetClusterID());

	if (bot->IsTestMode()) {
		/* Don't resume games in test mode */
		return true;
	}

	for (auto game = active.begin(); game != active.end(); ++game) {

		int64_t guild_id = from_string<int64_t>((*game)["guild_id"].get<std::string>(), std::dec);

		bool quickfire = (*game)["quickfire"].get<std::string>() == "1";

		/* XXX: Note: The mutex here is VITAL to thread safety of the state list! DO NOT move it! */
		{
			std::lock_guard<std::mutex> user_cache_lock(states_mutex);
			state_t* state = new state_t(this);
			state->start_time = time(NULL);

			/* Get shuffle list from state */
			if (!(*game)["qlist"].get<std::string>().empty()) {
				json shuffle = json::parse((*game)["qlist"].get<std::string>());
	
				for (auto s = shuffle.begin(); s != shuffle.end(); ++s) {
					state->shuffle_list.push_back(s->get<std::string>());
				}
				bot->core.log->debug("Resume shuffle list length: {}", state->shuffle_list.size());
				state->gamestate = (trivia_state_t)from_string<uint32_t>((*game)["state"].get<std::string>(), std::dec);
			} else {
				/* No shuffle list to resume from, create a new one */
				state->shuffle_list = fetch_shuffle_list(from_string<int64_t>((*game)["guild_id"].get<std::string>(), std::dec));
				state->gamestate = TRIV_ASK_QUESTION;
			}
	
			state->numquestions = from_string<uint32_t>((*game)["questions"].get<std::string>(), std::dec) + 1;
			state->streak = from_string<uint32_t>((*game)["streak"].get<std::string>(), std::dec);
			state->last_to_answer = from_string<int64_t>((*game)["lastanswered"].get<std::string>(), std::dec);
			state->round = from_string<uint32_t>((*game)["question_index"].get<std::string>(), std::dec);
			state->interval = (quickfire ? (TRIV_INTERVAL / 4) : TRIV_INTERVAL);
			state->channel_id = from_string<int64_t>((*game)["channel_id"].get<std::string>(), std::dec);
			state->guild_id = guild_id;
			state->curr_qid = 0;
			state->curr_answer = "";
			/* Force fetching of question */
			if (state->round % 10 == 0) {
				do_insane_round(state, true);
			} else {
				do_normal_round(state, true);
			}
			aegis::channel* c = bot->core.find_channel(state->channel_id);
			states[state->channel_id] = state;
			if (c) {
				bot->core.log->info("Resumed game on guild {}, channel {}, {} questions [{}]", state->guild_id, state->channel_id, state->numquestions, quickfire ? "quickfire" : "normal");
				state->timer = new std::thread(&state_t::tick, state);
			} else {
				state->terminating = true;
			}
		}
	}
	return true;
}

bool TriviaModule::OnChannelDelete(const modevent::channel_delete &cd)
{
	return true;
}

bool TriviaModule::OnGuildDelete(const modevent::guild_delete &gd)
{
	return true;
}

int64_t TriviaModule::GetActiveLocalGames()
{
	/* Counts local games running on this cluster only */
	int64_t a = 0;
	std::lock_guard<std::mutex> user_cache_lock(states_mutex);
	for (auto state = states.begin(); state != states.end(); ++state) {
		if (state->second && state->second->gamestate != TRIV_END && !state->second->terminating) {
			++a;
		}
	}
	return a;
}

int64_t TriviaModule::GetActiveGames()
{
	/* Counts all games across all clusters */
	auto rs = db::query("SELECT SUM(games) AS games FROM infobot_discord_counts WHERE dev = ?", {bot->IsDevMode() ? 1 : 0});
	if (rs.size()) {
		return from_string<int64_t>(rs[0]["games"], std::dec);
	} else {
		return 0;
	}
}

int64_t TriviaModule::GetGuildTotal()
{
	/* Counts all games across all clusters */
	auto rs = db::query("SELECT SUM(server_count) AS server_count FROM infobot_discord_counts WHERE dev = ?", {bot->IsDevMode() ? 1 : 0});
	if (rs.size()) {
		return from_string<int64_t>(rs[0]["server_count"], std::dec);
	} else {
		return 0;
	}
}

int64_t TriviaModule::GetMemberTotal()
{
	/* Counts all games across all clusters */
	auto rs = db::query("SELECT SUM(user_count) AS user_count FROM infobot_discord_counts WHERE dev = ?", {bot->IsDevMode() ? 1 : 0});
	if (rs.size()) {
		return from_string<int64_t>(rs[0]["user_count"], std::dec);
	} else {
		return 0;
	}
}

int64_t TriviaModule::GetChannelTotal()
{
	/* Counts all games across all clusters */
	auto rs = db::query("SELECT SUM(channel_count) AS channel_count FROM infobot_discord_counts WHERE dev = ?", {bot->IsDevMode() ? 1 : 0});
	if (rs.size()) {
		return from_string<int64_t>(rs[0]["channel_count"], std::dec);
	} else {
		return 0;
	}
}



guild_settings_t TriviaModule::GetGuildSettings(int64_t guild_id)
{
	aegis::guild* guild = bot->core.find_guild(guild_id);
	if (guild == nullptr) {
		return guild_settings_t(guild_id, "!", {}, 3238819, false, false, false, 0, "", "en", 20);
	} else {
		db::resultset r = db::query("SELECT * FROM bot_guild_settings WHERE snowflake_id = ?", {guild_id});
		if (!r.empty()) {
			std::stringstream s(r[0]["moderator_roles"]);
			int64_t role_id;
			std::vector<int64_t> role_list;
			while ((s >> role_id)) {
				role_list.push_back(role_id);
			}
			return guild_settings_t(from_string<int64_t>(r[0]["snowflake_id"], std::dec), r[0]["prefix"], role_list, from_string<uint32_t>(r[0]["embedcolour"], std::dec), (r[0]["premium"] == "1"), (r[0]["only_mods_stop"] == "1"), (r[0]["role_reward_enabled"] == "1"), from_string<int64_t>(r[0]["role_reward_id"], std::dec), r[0]["custom_url"], r[0]["language"], from_string<uint32_t>(r[0]["question_interval"], std::dec));
		} else {
			db::query("INSERT INTO bot_guild_settings (snowflake_id) VALUES('?')", {guild_id});
			return guild_settings_t(guild_id, "!", {}, 3238819, false, false, false, 0, "", "en", 20);
		}
	}
}

std::string TriviaModule::GetVersion()
{
	/* NOTE: This version string below is modified by a pre-commit hook on the git repository */
	std::string version = "$ModVer 39$";
	return "3.0." + version.substr(8,version.length() - 9);
}

std::string TriviaModule::GetDescription()
{
	return "Trivia System";
}

void TriviaModule::UpdatePresenceLine()
{
	while (!terminating) {
		sleep(20);
		int32_t questions = get_total_questions();
		bot->counters["activegames"] = GetActiveLocalGames();
		std::string presence = fmt::format("Trivia! {} questions, {} active games on {} servers through {} shards, cluster {}", Comma(questions), Comma(GetActiveGames()), Comma(this->GetGuildTotal()), Comma(bot->core.shard_max_count), bot->GetClusterID());
		bot->core.log->debug("PRESENCE: {}", presence);
		/* Can't translate this, it's per-shard! */
		bot->core.update_presence(presence, aegis::gateway::objects::activity::Game);

		if (!bot->IsTestMode()) {
			/* Don't handle shard reconnects or queued starts in test mode */
			CheckForQueuedStarts();
			CheckReconnects();
		}
	}
}

std::string TriviaModule::letterlong(std::string text, const guild_settings_t &settings)
{
	text = ReplaceString(text, " ", "");
	if (text.length()) {
		return fmt::format(_("HINT_LETTERLONG", settings), text.length(), text[0], text[text.length() - 1]);
	} else {
		return "An empty answer";
	}
}

std::string TriviaModule::vowelcount(std::string text, const guild_settings_t &settings)
{
	text = ReplaceString(lowercase(text), " ", "");
	int v = 0;
	for (auto x = text.begin(); x != text.end(); ++x) {
		if (isVowel(*x)) {
			++v;
		}
	}
	return fmt::format(_("HINT_VOWELCOUNT", settings), text.length(), v);
}

void TriviaModule::do_insane_round(state_t* state, bool silent)
{
	bot->core.log->debug("do_insane_round: G:{} C:{}", state->guild_id, state->channel_id);

	if (state->round >= state->numquestions) {
		state->gamestate = TRIV_END;
		state->score = 0;
		return;
	}

	guild_settings_t settings = GetGuildSettings(state->guild_id);

	std::vector<std::string> answers = fetch_insane_round(state->curr_qid, state->guild_id);
	if (log_question_index(state->guild_id, state->channel_id, state->round, state->streak, state->last_to_answer, state->gamestate)) {
		StopGame(state, settings);
		return;
	}

	state->insane = {};
	for (auto n = answers.begin(); n != answers.end(); ++n) {
		if (n == answers.begin()) {
			state->curr_question = trim(*n);
		} else {
			if (*n != "***END***") {
				state->insane[lowercase(trim(*n))] = true;
			}
		}
	}
	state->insane_left = state->insane.size();
	state->insane_num = state->insane.size();
	state->gamestate = TRIV_FIRST_HINT;

	aegis::channel* c = bot->core.find_channel(state->channel_id);
	if (c) {
		EmbedWithFields(fmt::format(_("QUESTION_COUNTER", settings), state->round, state->numquestions - 1), {{_("INSANE_ROUND", settings), fmt::format(_("INSANE_ANS_COUNT", settings), state->insane_num), false}, {_("QUESTION", settings), state->curr_question, false}}, c->get_id().get(), fmt::format("https://triviabot.co.uk/report/?c={}&g={}&insane={}", state->channel_id, state->guild_id, state->curr_qid + state->channel_id));
	} else {
		bot->core.log->warn("do_insane_round(): Channel {} was deleted", state->channel_id);
	}
}

void TriviaModule::do_normal_round(state_t* state, bool silent)
{
	bot->core.log->debug("do_normal_round: G:{} C:{}", state->guild_id, state->channel_id);

	if (state->round >= state->numquestions) {
		state->gamestate = TRIV_END;
		state->score = 0;
		return;
	}

	bool valid = false;
	int32_t tries = 0;

	guild_settings_t settings = GetGuildSettings(state->guild_id);

	do {
		state->curr_qid = 0;
		bot->core.log->debug("do_normal_round: fetch_question: '{}'", state->shuffle_list[state->round - 1]);
		std::vector<std::string> data = fetch_question(from_string<int64_t>(state->shuffle_list[state->round - 1], std::dec), state->guild_id);
		if (data.size() >= 12) {
			state->curr_qid = from_string<int64_t>(data[0], std::dec);
			state->curr_question = data[1];
			state->curr_answer = data[2];
			state->curr_customhint1 = data[3];
			state->curr_customhint2 = data[4];
			state->curr_category = data[5];
			state->curr_lastasked = from_string<time_t>(data[6],std::dec);
			state->curr_timesasked = from_string<int32_t>(data[7], std::dec);
			state->curr_lastcorrect = data[8];
			state->recordtime = from_string<time_t>(data[9],std::dec);
			state->shuffle1 = data[10];
			state->shuffle2 = data[11];
			valid = !state->curr_question.empty();
			if (!valid) {
				state->curr_qid = 0;
				bot->core.log->debug("do_normal_round: Invalid question response size {} with empty question retrieving question {}, round {} shuffle size {}", data.size(), state->shuffle_list[state->round - 1], state->round - 1, state->shuffle_list.size());
				sleep(2);
				tries++;
			}
		} else {
			bot->core.log->debug("do_normal_round: Invalid question response size {} retrieving question {}, round {} shuffle size {}", data.size(), state->shuffle_list[state->round - 1], state->round - 1, state->shuffle_list.size());
			sleep(2);
			state->curr_qid = 0;
			tries++;
			valid = false;
		}
	} while (!valid && tries <= 3);

	if (state->curr_qid == 0) {
		state->gamestate = TRIV_END;
		state->score = 0;
		state->curr_answer = "";
		bot->core.log->warn("do_normal_round(): Attempted to retrieve question id {} but got a malformed response. Round was aborted.", state->shuffle_list[state->round - 1]);
		aegis::channel* c = bot->core.find_channel(state->channel_id);
		if (c && !silent) {
			EmbedWithFields(fmt::format(_("Q_FETCH_ERROR", settings)), {{_("Q_SPOOPY", settings), _("Q_CONTACT_DEVS", settings), false}, {_("ROUND_STOPPING", settings), _("ERROR_BROKE_IT", settings), false}}, c->get_id().get());
		}
		return;
	}

	if (state->curr_question != "") {
		state->asktime = time(NULL);
		guild_settings_t settings = GetGuildSettings(state->guild_id);
		state->curr_answer = trim(state->curr_answer);
		std::string t = conv_num(state->curr_answer, settings);
		if (is_number(t) && t != "0") {
			state->curr_answer = t;
		}
		state->curr_answer = tidy_num(state->curr_answer);
		/* Handle hints */
		if (state->curr_customhint1.empty()) {
			/* No custom first hint, build one */
			state->curr_customhint1 = state->curr_answer;
			if (is_number(state->curr_customhint1)) {
				state->curr_customhint1 = MakeFirstHint(state->curr_customhint1, settings);
			} else {
				int32_t r = random(1, 12);
				if (r <= 4) {
					/* Leave only capital letters */
					for (int x = 0; x < state->curr_customhint1.length(); ++x) {
						if ((state->curr_customhint1[x] >= 'a' && state->curr_customhint1[x] <= 'z') || state->curr_customhint1[x] == '1' || state->curr_customhint1[x] == '3' || state->curr_customhint1[x] == '5' || state->curr_customhint1[x]  == '7' || state->curr_customhint1[x] == '9') {
							state->curr_customhint1[x] = '#';
						}
					}
				} else if (r >= 5 && r <= 8) {
					state->curr_customhint1 = letterlong(state->curr_customhint1, settings);
				} else {
					state->curr_customhint1 = fmt::format(_("SCRAMBLED_ANSWER", settings), state->shuffle1);
				}
			}
		}
		if (state->curr_customhint2.empty()) {
			/* No custom second hint, build one */
			state->curr_customhint2 = state->curr_answer;
			if (is_number(state->curr_customhint2) || PCRE("^\\$(\\d+)$").Match(state->curr_customhint2)) {
				std::string currency;
				std::vector<std::string> matches;
				if (PCRE("^\\$(\\d+)$").Match(state->curr_customhint2, matches)) {
					state->curr_customhint2 = matches[1];
					currency = "$";
				}
				state->curr_customhint2 = currency + state->curr_customhint2;
				int32_t r = random(1, 13);
				if ((r < 3 && from_string<int32_t>(state->curr_customhint2, std::dec) <= 10000)) {
					state->curr_customhint2 = dec_to_roman(from_string<unsigned int>(state->curr_customhint2, std::dec), settings);
				} else if ((r >= 3 && r < 6) || from_string<int32_t>(state->curr_customhint2, std::dec) > 10000) {
					state->curr_customhint2 = fmt::format(_("HEX", settings), from_string<int32_t>(state->curr_customhint2, std::dec));
				} else if (r >= 6 && r <= 10) {
					state->curr_customhint2 = fmt::format(_("OCT", settings), from_string<int32_t>(state->curr_customhint2, std::dec));
				} else {
					state->curr_customhint2 = fmt::format(_("BIN", settings), from_string<int32_t>(state->curr_customhint2, std::dec));
				}
			} else {
				int32_t r = random(1, 12);
				if (r <= 4) {
					/* Transpose only the vowels */
					for (int x = 0; x < state->curr_customhint2.length(); ++x) {
						if (toupper(state->curr_customhint2[x]) == 'A' || toupper(state->curr_customhint2[x]) == 'E' || toupper(state->curr_customhint2[x]) == 'I' || toupper(state->curr_customhint2[x]) == 'O' || toupper(state->curr_customhint2[x]) == 'U' || toupper(state->curr_customhint2[x]) == '2' || toupper(state->curr_customhint2[x]) == '4' || toupper(state->curr_customhint2[x]) == '6' || toupper(state->curr_customhint2[x]) == '8' || toupper(state->curr_customhint2[x]) == '0') {
							state->curr_customhint2[x] = '#';
						}
					}
				} else if ((r >= 5 && r <= 6) || settings.language != "en") {
					state->curr_customhint2 = vowelcount(state->curr_customhint2, settings);
				} else {
					/* settings.language check for en above, because piglatin only makes sense in english */
					state->curr_customhint2 = piglatin(state->curr_customhint2);
				}

			}
		}

		aegis::channel* c = bot->core.find_channel(state->channel_id);
		if (c) {
			if (!silent) {
				EmbedWithFields(fmt::format(_("QUESTION_COUNTER", settings), state->round, state->numquestions - 1), {{_("CATEGORY", settings), state->curr_category, false}, {_("QUESTION", settings), state->curr_question, false}}, c->get_id().get(), fmt::format("https://triviabot.co.uk/report/?c={}&g={}&normal={}", state->channel_id, state->guild_id, state->curr_qid + state->channel_id));
			}
		} else {
			bot->core.log->warn("do_normal_round(): Channel {} was deleted", state->channel_id);
		}

	} else {
		aegis::channel* c = bot->core.find_channel(state->channel_id);
		if (c) {
			if (!silent) {
				SimpleEmbed(":ghost:", _("BRAIN_BROKE_IT", settings), c->get_id().get(), _("FETCH_Q", settings));
			}
		} else {
			bot->core.log->debug("do_normal_round: G:{} C:{} channel vanished! -- question with no text!", state->guild_id, state->channel_id);
		}
	}

	state->score = (state->interval == TRIV_INTERVAL ? 4 : 8);
	/* Advance state to first hint */
	state->gamestate = TRIV_FIRST_HINT;
	if (log_question_index(state->guild_id, state->channel_id, state->round, state->streak, state->last_to_answer, state->gamestate)) {
		StopGame(state, settings);
	}

}

void TriviaModule::do_first_hint(state_t* state)
{
	bot->core.log->debug("do_first_hint: G:{} C:{}", state->guild_id, state->channel_id);
	aegis::channel* c = bot->core.find_channel(state->channel_id);
	guild_settings_t settings = GetGuildSettings(state->guild_id);
	if (c) {
		if (state->round % 10 == 0) {
			/* Insane round countdown */
			SimpleEmbed(":clock10:", fmt::format(_("SECS_LEFT", settings), state->interval * 2), c->get_id().get());
		} else {
			/* First hint, not insane round */
			SimpleEmbed(":clock10:", state->curr_customhint1, c->get_id().get(), _("FIRST_HINT", settings));
		}
	} else {
		 bot->core.log->warn("do_first_hint(): Channel {} was deleted", state->channel_id);
	}
	state->gamestate = TRIV_SECOND_HINT;
	state->score = (state->interval == TRIV_INTERVAL ? 2 : 4);
	if (log_question_index(state->guild_id, state->channel_id, state->round, state->streak, state->last_to_answer, state->gamestate)) {
		StopGame(state, settings);
	}
}

void TriviaModule::do_second_hint(state_t* state)
{
	bot->core.log->debug("do_second_hint: G:{} C:{}", state->guild_id, state->channel_id);
	aegis::channel* c = bot->core.find_channel(state->channel_id);
	guild_settings_t settings = GetGuildSettings(state->guild_id);
	if (c) {
		if (state->round % 10 == 0) {
			/* Insane round countdown */
			SimpleEmbed(":clock1030:", fmt::format(_("SECS_LEFT", settings), state->interval), c->get_id().get());
		} else {
			/* Second hint, not insane round */
			SimpleEmbed(":clock1030:", state->curr_customhint2, c->get_id().get(), _("SECOND_HINT", settings));
		}
	} else {
		 bot->core.log->warn("do_second_hint: Channel {} was deleted", state->channel_id);
	}
	state->gamestate = TRIV_TIME_UP;
	state->score = (state->interval == TRIV_INTERVAL ? 1 : 2);
	if (log_question_index(state->guild_id, state->channel_id, state->round, state->streak, state->last_to_answer, state->gamestate)) {
		StopGame(state, settings);
	}
}

void TriviaModule::do_time_up(state_t* state)
{
	bot->core.log->debug("do_time_up: G:{} C:{}", state->guild_id, state->channel_id);
	aegis::channel* c = bot->core.find_channel(state->channel_id);
	guild_settings_t settings = GetGuildSettings(state->guild_id);

	if (c) {
		if (state->round % 10 == 0) {
			int32_t found = state->insane_num - state->insane_left;
			SimpleEmbed(":alarm_clock:", fmt::format(_("INSANE_FOUND", settings), found), c->get_id().get(), _("TIME_UP", settings));
		} else if (state->curr_answer != "") {
			SimpleEmbed(":alarm_clock:", fmt::format(_("ANS_WAS", settings), state->curr_answer), c->get_id().get(), _("OUT_OF_TIME", settings));
		}
	}
	/* FIX: You can only lose your streak on a non-insane round */
	if (state->curr_answer != "" && state->round % 10 != 0 && state->streak > 1 && state->last_to_answer) {
		aegis::user* u = bot->core.find_user(state->last_to_answer);
		if (u) {
			SimpleEmbed(":octagonal_sign:", fmt::format(_("STREAK_SMASHED", settings), u->get_username(), state->streak), c->get_id().get());
		}
	}

	if (state->curr_answer != "") {
		state->curr_answer = "";
		if (state->round % 10 != 0) {
			state->last_to_answer = 0;
			state->streak = 1;
		}
	}

	if (c && state->round <= state->numquestions - 2) {
		SimpleEmbed("<a:loading:658667224067735562>", fmt::format(_("COMING_UP", settings), state->interval == TRIV_INTERVAL ? settings.question_interval : state->interval), c->get_id().get(), _("REST", settings));
	}

	state->gamestate = (state->round > state->numquestions ? TRIV_END : TRIV_ASK_QUESTION);
	state->round++;
	state->score = 0;
	if (log_question_index(state->guild_id, state->channel_id, state->round, state->streak, state->last_to_answer, state->gamestate)) {
		StopGame(state, settings);
	}
}

void TriviaModule::do_answer_correct(state_t* state)
{
	bot->core.log->debug("do_answer_correct: G:{} C:{}", state->guild_id, state->channel_id);

	aegis::channel* c = bot->core.find_channel(state->channel_id);
	guild_settings_t settings = GetGuildSettings(state->guild_id);

	state->round++;
	state->score = 0;

	if (state->round <= state->numquestions - 2) {
		if (c) {
			SimpleEmbed("<a:loading:658667224067735562>", fmt::format(_("COMING_UP", settings), state->interval), c->get_id().get(), _("REST", settings));
		} else {
			bot->core.log->warn("do_answer_correct(): Channel {} was deleted", state->channel_id);
		}
	}
	state->gamestate = TRIV_ASK_QUESTION;
	if (log_question_index(state->guild_id, state->channel_id, state->round, state->streak, state->last_to_answer, state->gamestate)) {
		StopGame(state, settings);
	}
}

void TriviaModule::do_end_game(state_t* state)
{
	bot->core.log->debug("do_end_game: G:{} C:{}", state->guild_id, state->channel_id);

	log_game_end(state->guild_id, state->channel_id);

	aegis::channel* c = bot->core.find_channel(state->channel_id);
	if (c) {
		guild_settings_t settings = GetGuildSettings(state->guild_id);
		bot->core.log->info("End of game on guild {}, channel {} after {} seconds", state->guild_id, state->channel_id, time(NULL) - state->start_time);
		SimpleEmbed(":stop_button:", fmt::format(_("END1", settings), state->numquestions - 1), c->get_id().get(), _("END_TITLE", settings));
		show_stats(c->get_guild().get_id(), state->channel_id);
	} else {
		bot->core.log->warn("do_end_game(): Channel {} was deleted", state->channel_id);
	}
	state->terminating = true;
}

void TriviaModule::show_stats(int64_t guild_id, int64_t channel_id)
{
	std::vector<std::string> topten = get_top_ten(guild_id);
	size_t count = 1;
	std::string msg;
	std::vector<field_t> fields;
	for(auto r = topten.begin(); r != topten.end(); ++r) {
		std::stringstream score(*r);
		std::string points;
		int64_t snowflake_id;
		score >> points;
		score >> snowflake_id;
		aegis::user* u = bot->core.find_user(snowflake_id);
		if (u) {
			msg.append(fmt::format("{}. ``{}`` ({})\n", count++, u->get_full_name(), points));
		} else {
			msg.append(fmt::format("{}. ``Deleted User#0000`` ({})\n", count++, points));
		}
	}
	if (msg.empty()) {
		msg = "Nobody has played here today! :cry:";
	}
	aegis::channel* c = bot->core.find_channel(channel_id);
	if (c) {
		guild_settings_t settings = GetGuildSettings(guild_id);
		if (settings.premium && !settings.custom_url.empty()) {
			EmbedWithFields(_("LEADERBOARD", settings), {{_("TOP_TEN", settings), msg, false}, {_("MORE_INFO", settings), fmt::format(_("LEADER_LINK", settings), settings.custom_url), false}}, c->get_id().get());
		} else {
			EmbedWithFields(_("LEADERBOARD", settings), {{_("TOP_TEN", settings), msg, false}, {_("MORE_INFO", settings), fmt::format(_("LEADER_LINK", settings), guild_id), false}}, c->get_id().get());
		}
	}
}

void TriviaModule::Tick(state_t* state)
{
	if (state->terminating) {
		return;
	}

	uint32_t waits = 0;
	while ((bot->core.find_guild(state->guild_id) == nullptr || bot->core.find_channel(state->channel_id) == nullptr) && !state->terminating) {
		bot->core.log->warn("Guild or channel are missing!!! Waiting 5 seconds for connection to re-establish to guild/channel: G:{} C:{}", state->guild_id, state->channel_id);
		sleep(5);
		if (waits++ > 30) {
			bot->core.log->warn("Waited too long for re-connection of G:{} C:{}, ending round.", state->guild_id, state->channel_id);
			state->gamestate = TRIV_END;
			state->terminating = true;
		}
	}
	

	if (!state->terminating) {
		switch (state->gamestate) {
			case TRIV_ASK_QUESTION:
				if (state->round % 10 == 0) {
					do_insane_round(state, false);
				} else {
					do_normal_round(state, false);
				}
			break;
			case TRIV_FIRST_HINT:
				do_first_hint(state);
			break;
			case TRIV_SECOND_HINT:
				do_second_hint(state);
			break;
			case TRIV_TIME_UP:
				do_time_up(state);
			break;
			case TRIV_ANSWER_CORRECT:
				do_answer_correct(state);
			break;
				case TRIV_END:
				do_end_game(state);
			break;
			default:
				bot->core.log->warn("Invalid state '{}', ending round.", state->gamestate);
				state->gamestate = TRIV_END;
				state->terminating = true;
			break;
		}
	}
}

void TriviaModule::DisposeThread(std::thread* t)
{
	bot->DisposeThread(t);
}

void TriviaModule::StopGame(state_t* state, const guild_settings_t &settings)
{
	if (state->gamestate != TRIV_END) {
		SimpleEmbed(":octagonal_sign:", _("DASH_STOP", settings), state->channel_id, _("STOPPING", settings));
		state->gamestate = TRIV_END;
		state->terminating = false;
	}
}

void TriviaModule::CheckForQueuedStarts()
{
	db::resultset rs = db::query("SELECT * FROM start_queue ORDER BY queuetime", {});
	for (auto r = rs.begin(); r != rs.end(); ++r) {
		int64_t guild_id = from_string<int64_t>((*r)["guild_id"], std::dec);
		/* Check that this guild is on this cluster, if so we can start this game */
		if (bot->core.find_guild(guild_id)) {
			int64_t channel_id = from_string<int64_t>((*r)["channel_id"], std::dec);
			int64_t user_id = from_string<int64_t>((*r)["user_id"], std::dec);
			int32_t questions = from_string<int32_t>((*r)["questions"], std::dec);
			int32_t quickfire = from_string<int32_t>((*r)["quickfire"], std::dec);
			bot->core.log->info("Remote start, guild_id={} channel_id={} user_id={} questions={} type={}", guild_id, channel_id, user_id, questions, quickfire ? "quickfire" : "normal");
			guild_settings_t settings = GetGuildSettings(guild_id);
			aegis::gateway::objects::message m(fmt::format("{}{} {}", settings.prefix, (quickfire ? "quickfire" : "start"), questions), bot->core.find_channel(channel_id), bot->core.find_guild(guild_id));

			struct modevent::message_create msg = {
				*(bot->core.get_shard_mgr().get_shards()[0]),
				*(bot->core.find_user(user_id)),
				*(bot->core.find_channel(channel_id)),
				m
			};

			RealOnMessage(msg, msg.msg.get_content(), false, {}, user_id);
	
			db::query("DELETE FROM start_queue WHERE channel_id = ?", {channel_id});
		}
	}
}

void TriviaModule::CacheUser(int64_t user, int64_t channel_id)
{
	aegis::channel* c = bot->core.find_channel(channel_id);
	aegis::user* _user = bot->core.find_user(user);
	if (_user && c) {
		aegis::user::guild_info& gi = _user->get_guild_info(c->get_guild().get_id());
		cache_user(_user, &c->get_guild(), &gi);
	} else {
		bot->core.log->debug("Command with no user!");
	}
}

bool TriviaModule::OnMessage(const modevent::message_create &message, const std::string& clean_message, bool mentioned, const std::vector<std::string> &stringmentions)
{
	return RealOnMessage(message, clean_message, mentioned, stringmentions, 0);
}

bool TriviaModule::RealOnMessage(const modevent::message_create &message, const std::string& clean_message, bool mentioned, const std::vector<std::string> &stringmentions, int64_t _author_id)
{
	std::string username;
	aegis::gateway::objects::message msg = message.msg;

	// Allow overriding of author id from remote start code
	int64_t author_id = _author_id ? _author_id : msg.get_author_id().get();

	bool isbot = msg.author.is_bot();

	if (!message.has_user()) {
		bot->core.log->debug("Message has no user! Message id {} author id {}", msg.get_id().get(), author_id);
		return true;
	}

	aegis::user* user = bot->core.find_user(author_id);
	if (user) {
		username = user->get_username();
		if (isbot) {
			/* Drop bots here */
			return true;
		}
	}
	
	/* Retrieve current state for channel, if there is no state object, no game is in progress */
	state_t* state = nullptr;
	int64_t guild_id = 0;
	aegis::channel* c = nullptr;

	if (msg.has_channel()) {
		if (msg.get_channel_id().get() == 0) {
			c = bot->core.find_channel(msg.get_channel().get_id().get());
		} else {
			c = bot->core.find_channel(msg.get_channel_id().get());
		}
		if (c) {
			guild_id = c->get_guild().get_id().get();
		} else {
			bot->core.log->warn("Channel is missing!!! C:{} A:{}", msg.get_channel_id().get(), author_id);
			return true;
		}
	} else {
		/* No channel! */
		bot->core.log->debug("Message without channel, M:{} A:{}", msg.get_id().get(), author_id);
		return true;
	}

	int64_t channel_id = c->get_id().get();

	guild_settings_t settings = GetGuildSettings(guild_id);

	std::string trivia_message = clean_message;
	int x = from_string<int>(conv_num(clean_message, settings), std::dec);
	if (x > 0) {
		trivia_message = conv_num(clean_message, settings);
	}
	trivia_message = tidy_num(trivia_message);
	state = GetState(channel_id);

	if (mentioned && prefix_match->Match(clean_message)) {
		c->create_message(fmt::format(_("PREFIX", settings), settings.prefix, settings.prefix));
		bot->core.log->debug("Respond to prefix request on channel C:{} A:{}", channel_id, author_id);
		return false;
	}

	if (lowercase(clean_message.substr(0, settings.prefix.length())) == lowercase(settings.prefix)) {
		/* Command */

		std::string command = clean_message.substr(settings.prefix.length(), clean_message.length() - settings.prefix.length());
		if (user != nullptr) {
			queue_command(command, author_id, channel_id, guild_id, mentioned);
			bot->core.log->info("CMD (USER={}, GUILD={}): <{}> {}", author_id, guild_id, username, clean_message);
			return false;
		} else {
			bot->core.log->debug("User is null when handling command. C:{} A:{}", channel_id, author_id);
		}

	} else if (state) {
		/* The state_t class handles potential answers, but only when a game is running on this guild */
		state->queue_message(trivia_message, author_id, mentioned);
		bot->core.log->debug("Processed potential answer message from A:{} on C:{}", author_id, channel_id);
	}

	return true;
}


state_t* TriviaModule::GetState(int64_t channel_id) {
	std::lock_guard<std::mutex> user_cache_lock(states_mutex);
	state_t* state = nullptr;

	std::vector<int64_t> removals;
	for (auto& s : states) {
		if (s.second && s.second->terminating) {
			removals.push_back(s.first);
			delete s.second;
		} else if (!s.second) {
			removals.push_back(s.first);
		}
	}
	for (auto r : removals) {
		states.erase(r);
	}

	auto state_iter = states.find(channel_id);
	if (state_iter != states.end()) {
		state = state_iter->second;
	}

	return state;
}

ENTRYPOINT(TriviaModule);

