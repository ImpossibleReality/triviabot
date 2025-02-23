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
#include <sporks/modules.h>
#include <sporks/stringops.h>
#include <sporks/statusfield.h>
#include <sporks/database.h>
#include "trivia.h"

/* Check for shards we have been asked to reconnect */
void TriviaModule::CheckReconnects() {
	db::resultset rs = db::query("SELECT * FROM infobot_shard_status WHERE forcereconnect = 1 AND cluster_id = ?", {bot->GetClusterID()});
	if (!rs.empty()) {
		for (auto r = rs.begin(); r != rs.end(); ++r) {
			try {
				auto & s = bot->core.get_shard_by_id(from_string<uint32_t>((*r)["id"], std::dec));
				bot->core.get_shard_mgr().close(s);
				sleep(2);
				s.connect();
			}
			catch (...) {
				bot->core.log->error("Unable to get shard {} to reconnect it! Something broked!", (*r)["id"]);
			}
			db::query("UPDATE infobot_shard_status SET forcereconnect = 0 WHERE id = '?'", {(*r)["id"]});
		}
	}
}

