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
#include "commands.h"

command_create_t::command_create_t(class TriviaModule* _creator, const std::string &_base_command) : command_t(_creator, _base_command) { }

void command_create_t::call(const in_cmd &cmd, std::stringstream &tokens, guild_settings_t &settings, const std::string &username, bool is_moderator, aegis::channel* c, aegis::user* user, state_t* state)
{
	std::string newteamname;
	std::getline(tokens, newteamname);
	newteamname = trim(newteamname);
	std::string teamname = get_current_team(cmd.author_id);
	if (teamname.empty() || teamname == "!NOTEAM") {
		newteamname = create_new_team(newteamname);
		if (newteamname != "__NO__") {
			join_team(cmd.author_id, newteamname, cmd.channel_id);
			creator->SimpleEmbed(":busts_in_silhouette:", fmt::format(_("CREATED", settings), newteamname, username), c->get_id().get(), _("ZELDAREFERENCE", settings));
		} else {
			creator->SimpleEmbed(":warning:", fmt::format(_("CANTCREATE", settings), username), cmd.channel_id);
		}
	} else {
		creator->SimpleEmbed(":warning:", fmt::format(_("ALREADYMEMBER", settings), username, teamname), cmd.channel_id);
	}
	creator->CacheUser(cmd.author_id, cmd.channel_id);
}

