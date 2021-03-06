/*
 * Copyright (C) 2008-2014 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2011-2016 ArkCORE <http://www.arkania.net/>
 * Copyright (C) 2006-2009 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* ScriptData
SDName: Teldrassil
SD%Complete: 100
SDComment: Quest support: 938
SDCategory: Teldrassil
EndScriptData */

/* ContentData
npc_mist
EndContentData */

#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedFollowerAI.h"
#include "Player.h"

/*####
# npc_mist
####*/

enum Mist
{
    SAY_AT_HOME             = 0,
    EMOTE_AT_HOME           = 1,
    QUEST_MIST              = 938,
    NPC_ARYNIA              = 3519,
    FACTION_DARNASSUS       = 79
};

class npc_mist : public CreatureScript
{
public:
    npc_mist() : CreatureScript("npc_mist") { }

    bool OnQuestAccept(Player* player, Creature* creature, Quest const* quest) override
    {
        if (quest->GetQuestId() == QUEST_MIST)
            if (npc_mistAI* pMistAI = CAST_AI(npc_mist::npc_mistAI, creature->AI()))
                pMistAI->StartFollow(player, FACTION_DARNASSUS, quest);

        return true;
    }

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_mistAI(creature);
    }

    struct npc_mistAI : public FollowerAI
    {
        npc_mistAI(Creature* creature) : FollowerAI(creature) { }

        void Reset() override { }

        void MoveInLineOfSight(Unit* who) override

        {
            FollowerAI::MoveInLineOfSight(who);

            if (!me->GetVictim() && !HasFollowState(STATE_FOLLOW_COMPLETE) && who->GetEntry() == NPC_ARYNIA)
            {
                if (me->IsWithinDistInMap(who, 10.0f))
                {
                    Talk(SAY_AT_HOME, who);
                    DoComplete();
                }
            }
        }

        void DoComplete()
        {
            Talk(EMOTE_AT_HOME);

            Player* player = GetLeaderForFollower();
            if (player && player->GetQuestStatus(QUEST_MIST) == QUEST_STATUS_INCOMPLETE)
                player->GroupEventHappens(QUEST_MIST, me);

            //The follow is over (and for later development, run off to the woods before really end)
            SetFollowComplete();
        }

        //call not needed here, no known abilities
        /*void UpdateFollowerAI(const uint32 Diff) override
        {
            if (!UpdateVictim())
                return;

            DoMeleeAttackIfReady();
        }*/
    };

};
//fix aldrassil priest  Healing for the Wounded 26949  druid quest Rejuvenating Touch 26948
// npc wounded sentinel 44617
class npc_wounded_sentinel_44617 : public CreatureScript
{
public:
	npc_wounded_sentinel_44617() : CreatureScript("npc_wounded_sentinel_44617") {}

	enum eNpc
	{
		QUEST_A_REJUVENATING_TOUCH_26948 = 26948,
		QUEST_FLASH_HEAL_26949 = 26949,
		NPC_HEALING_CREDIT_44175 = 44175,
	};

	struct npc_wounded_sentinel_44617AI : public ScriptedAI
	{
		npc_wounded_sentinel_44617AI(Creature* creature) : ScriptedAI(creature) {}

		void SpellHit(Unit* caster, SpellInfo const* spell) override
		{
			if (Player* player = caster->ToPlayer())
				if (player->GetQuestStatus(QUEST_A_REJUVENATING_TOUCH_26948) == QUEST_STATUS_INCOMPLETE)
					player->KilledMonsterCredit(NPC_HEALING_CREDIT_44175);
				else if (player->GetQuestStatus(QUEST_FLASH_HEAL_26949) == QUEST_STATUS_INCOMPLETE)
					player->KilledMonsterCredit(NPC_HEALING_CREDIT_44175);
		}
	};

	CreatureAI* GetAI(Creature* creature) const
	{
		return new npc_wounded_sentinel_44617AI(creature);
	}
};
void AddSC_teldrassil()
{
    new npc_mist();
    new npc_wounded_sentinel_44617();
}
