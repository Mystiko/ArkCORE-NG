/*
 * Copyright (C) 2008-2014 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2011-2016 ArkCORE <http://www.arkania.net/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
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

#include "Common.h"
#include "Log.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "World.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "GossipDef.h"
#include "QuestDef.h"
#include "ObjectAccessor.h"
#include "Group.h"
#include "Battleground.h"
#include "ScriptMgr.h"
#include "GameObjectAI.h"

void WorldSession::HandleQuestgiverStatusQueryOpcode(WorldPacket& recvData)
{
    uint64 guid;
    recvData >> guid;
    uint32 questStatus = DIALOG_STATUS_NONE;

    Object* questGiver = ObjectAccessor::GetObjectByTypeMask(*_player, guid, TYPEMASK_UNIT | TYPEMASK_GAMEOBJECT);
    if (!questGiver)
    {
        TC_LOG_INFO("network", "Error in CMSG_QUESTGIVER_STATUS_QUERY, called for non-existing questgiver (Typeid: %u GUID: %u)", GuidHigh2TypeId(GUID_HIPART(guid)), GUID_LOPART(guid));
        return;
    }

    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUESTGIVER_STATUS_QUERY for npc, guid = %u", questGiver->GetGUIDLow());
            if (!questGiver->ToCreature()->IsHostileTo(_player)) // do not show quest status to enemies
                questStatus = _player->GetQuestDialogStatus(questGiver);
            break;
        }
        case TYPEID_GAMEOBJECT:
        {
            TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUESTGIVER_STATUS_QUERY for GameObject guid = %u", questGiver->GetGUIDLow());
            questStatus = _player->GetQuestDialogStatus(questGiver);
            break;
        }
        default:
            TC_LOG_ERROR("network", "QuestGiver called for unexpected type %u", questGiver->GetTypeId());
            break;
    }

    //inform client about status of quest
    _player->PlayerTalkClass->SendQuestGiverStatus(questStatus, guid);
}

void WorldSession::HandleQuestgiverHelloOpcode(WorldPacket& recvData)
{
    uint64 guid;
    recvData >> guid;

    TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUESTGIVER_HELLO npc = %u", GUID_LOPART(guid));

    Creature* creature = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
    if (!creature)
    {
        TC_LOG_DEBUG("network", "WORLD: HandleQuestgiverHelloOpcode - Unit (GUID: %u) not found or you can't interact with him.",
            GUID_LOPART(guid));
        return;
    }

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);
    // Stop the npc if moving
    creature->StopMoving();

    if (sScriptMgr->OnGossipHello(_player, creature))
        return;

    _player->PrepareGossipMenu(creature, creature->GetCreatureTemplate()->GossipMenuId, true);
    _player->SendPreparedGossip(creature);

    creature->AI()->sGossipHello(_player);
}

void WorldSession::HandleQuestgiverAcceptQuestOpcode(WorldPacket& recvData)
{
    uint64 guid;
    uint32 questId;
    uint32 unk1;
    recvData >> guid >> questId >> unk1;

    TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUESTGIVER_ACCEPT_QUEST npc = %u, quest = %u, unk1 = %u", uint32(GUID_LOPART(guid)), questId, unk1);

    Object* object = ObjectAccessor::GetObjectByTypeMask(*_player, guid, TYPEMASK_UNIT|TYPEMASK_GAMEOBJECT|TYPEMASK_ITEM|TYPEMASK_PLAYER);

#define CLOSE_GOSSIP_CLEAR_DIVIDER() \
    do { \
        _player->PlayerTalkClass->SendCloseGossip(); \
        _player->SetDivider(0); \
    } while (0)

    // no or incorrect quest giver
    if (!object)
    {
        CLOSE_GOSSIP_CLEAR_DIVIDER();
        return;
    }

    if (Player* playerQuestObject = object->ToPlayer())
    {
        if ((_player->GetDivider() && _player->GetDivider() != guid) ||
           ((object != _player && !playerQuestObject->CanShareQuest(questId))))
        {
            CLOSE_GOSSIP_CLEAR_DIVIDER();
            return;
        }
    }
    else
    {
        if (!object->hasQuest(questId))
        {
            CLOSE_GOSSIP_CLEAR_DIVIDER();
            return;
        }
    }

    // some kind of WPE protection
    if (!_player->CanInteractWithQuestGiver(object))
        return;

    if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
    {
        // prevent cheating
        if (!GetPlayer()->CanTakeQuest(quest, true))
        {
            CLOSE_GOSSIP_CLEAR_DIVIDER();
            return;
        }

        if (_player->GetDivider() != 0)
        {
            Player* player = ObjectAccessor::FindPlayer(_player->GetDivider());
            if (player)
            {
                player->SendPushToPartyResponse(_player, QUEST_PARTY_MSG_ACCEPT_QUEST);
                _player->SetDivider(0);
            }
        }

        if (_player->CanAddQuest(quest, true))
        {
            _player->AddQuestAndCheckCompletion(quest, object);

            if (quest->HasFlag(QUEST_FLAGS_PARTY_ACCEPT))
            {
                if (Group* group = _player->GetGroup())
                {
                    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                    {
                        Player* player = itr->GetSource();

                        if (!player || player == _player)     // not self
                            continue;

                        if (player->CanTakeQuest(quest, true))
                        {
                            player->SetDivider(_player->GetGUID());

                            // need confirmation that any gossip window will close
                            player->PlayerTalkClass->SendCloseGossip();

                            _player->SendQuestConfirmAccept(quest, player);
                        }
                    }
                }
            }

            _player->PlayerTalkClass->SendCloseGossip();

            if (quest->GetSrcSpell() > 0)
                _player->CastSpell(_player, quest->GetSrcSpell(), true);

            return;
        }

        if (_player)
            _player->SaveToDB(); // Save player.
    }

    _player->PlayerTalkClass->SendCloseGossip();

#undef CLOSE_GOSSIP_CLEAR_DIVIDER
}

void WorldSession::HandleQuestgiverQueryQuestOpcode(WorldPacket& recvData)
{
    uint64 guid;
    uint32 questId;
    uint8 unk1;
    recvData >> guid >> questId >> unk1;
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUESTGIVER_QUERY_QUEST npc = %u, quest = %u, unk1 = %u", uint32(GUID_LOPART(guid)), questId, unk1);

    // Verify that the guid is valid and is a questgiver or involved in the requested quest
    Object* object = ObjectAccessor::GetObjectByTypeMask(*_player, guid, TYPEMASK_UNIT | TYPEMASK_GAMEOBJECT | TYPEMASK_ITEM);
    if (!object || (!object->hasQuest(questId) && !object->hasInvolvedQuest(questId)))
    {
        _player->PlayerTalkClass->SendCloseGossip();
        return;
    }

    if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
    {
        // not sure here what should happen to quests with QUEST_FLAGS_AUTO_COMPLETE
        // if this breaks them, add && object->GetTypeId() == TYPEID_ITEM to this check
        // item-started quests never have that flag
        if (!_player->CanTakeQuest(quest, true))
            return;

        if (quest->IsAutoAccept() && _player->CanAddQuest(quest, true))
        {
            _player->AddQuestAndCheckCompletion(quest, object);
           // _player->PlayerTalkClass->SendCloseGossip();
        }

        if (quest->IsAutoComplete())
            _player->PlayerTalkClass->SendQuestGiverRequestItems(quest, object->GetGUID(), _player->CanCompleteQuest(quest->GetQuestId()), true);
        else
            _player->PlayerTalkClass->SendQuestGiverQuestDetails(quest, object->GetGUID(), quest->IsAutoAccept() ? false : true);

        if (_player)
            _player->SaveToDB(); // Save player.
    }
}

void WorldSession::HandleQuestQueryOpcode(WorldPacket& recvData)
{
    if (!_player)
        return;

    uint32 questId;
    recvData >> questId;
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUEST_QUERY quest = %u", questId);

    if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
        _player->PlayerTalkClass->SendQuestQueryResponse(quest);
}

void WorldSession::HandleQuestgiverChooseRewardOpcode(WorldPacket& recvData)
{
    uint32 questId, reward;
    uint64 guid;
    recvData >> guid >> questId >> reward;

    if (reward >= QUEST_REWARD_CHOICES_COUNT)
    {
        TC_LOG_ERROR("network", "Error in CMSG_QUESTGIVER_CHOOSE_REWARD: player %s (guid %d) tried to get invalid reward (%u) (possible packet-hacking detected)", _player->GetName().c_str(), _player->GetGUIDLow(), reward);
        return;
    }

    TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUESTGIVER_CHOOSE_REWARD npc = %u, quest = %u, reward = %u", uint32(GUID_LOPART(guid)), questId, reward);

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return;

    Object* object = _player;

    // if (!quest->HasFlag(QUEST_FLAGS_AUTO_COMPLETE)) // old arkcore or trinity
    // if (!quest->HasFlag(QUEST_FLAGS_AUTO_SUBMIT) && !quest->HasFlag(QUEST_FLAGS_AUTO_REWARDED)) // emucore
    if (GUID_HIPART(guid) != HIGHGUID_PLAYER)
    {
        object = ObjectAccessor::GetObjectByTypeMask(*_player, guid, TYPEMASK_UNIT | TYPEMASK_GAMEOBJECT | TYPEMASK_ITEM);
        if (!object || !object->hasInvolvedQuest(questId))
            return;

        // some kind of WPE protection
        if (!_player->CanInteractWithQuestGiver(object))
            return;
    }

    if ((!_player->CanSeeStartQuest(quest) && _player->GetQuestStatus(questId) == QUEST_STATUS_NONE) ||
        (_player->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE && !quest->IsAutoComplete()))
    {
        TC_LOG_ERROR("network", "Error in QUEST_STATUS_COMPLETE: player %s (guid %u) tried to complete quest %u, but is not allowed to do so (possible packet-hacking or high latency)",
            _player->GetName().c_str(), _player->GetGUIDLow(), questId);
        return;
    }

    if (_player->CanRewardQuest(quest, reward, true))
    {
        _player->RewardQuest(quest, reward, object);

        if (!_player->InformQuestGiverReward(quest, object, reward))
        {
            if (Quest const* nextQuest = _player->GetNextQuest(guid, quest))
            {
                if (nextQuest->IsAutoAccept() && _player->CanAddQuest(nextQuest, true) && _player->CanTakeQuest(nextQuest, true))
                {
                    if (nextQuest->IsAutoAccept() && _player->CanAddQuest(nextQuest, true) && _player->CanTakeQuest(nextQuest, true))
                        _player->AddQuestAndCheckCompletion(nextQuest, object);

                    _player->PlayerTalkClass->SendQuestGiverQuestDetails(nextQuest, guid, true);
                }

                _player->PlayerTalkClass->SendQuestGiverQuestDetails(nextQuest, guid, true);
            }
            else
            {
                // Don't forget to close window.
                _player->SendQuestWindowClose(quest->GetQuestId());
            }
        }
    }
    else
        _player->PlayerTalkClass->SendQuestGiverOfferReward(quest, guid, true);
}

void WorldSession::HandleQuestgiverRequestRewardOpcode(WorldPacket& recvData)
{
    uint32 questId;
    uint64 guid;
    recvData >> guid >> questId;

    TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUESTGIVER_REQUEST_REWARD npc = %u, quest = %u", uint32(GUID_LOPART(guid)), questId);

    if (guid != _player->GetGUID())
    {
        Object* object = ObjectAccessor::GetObjectByTypeMask(*_player, guid, TYPEMASK_UNIT | TYPEMASK_GAMEOBJECT);
        if (!object || !object->hasInvolvedQuest(questId))
            return;
        // some kind of WPE protection
        if (!_player->CanInteractWithQuestGiver(object))
            return;
    }

    if (_player->CanCompleteQuest(questId))
        _player->CompleteQuest(questId);

    if (_player->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
        return;
   
    if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
        _player->PlayerTalkClass->SendQuestGiverOfferReward(quest, guid, true);
}

void WorldSession::HandleQuestgiverCancel(WorldPacket& /*recvData*/)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUESTGIVER_CANCEL");

    _player->PlayerTalkClass->SendCloseGossip();
}

void WorldSession::HandleQuestLogSwapQuest(WorldPacket& recvData)
{
    uint8 slot1, slot2;
    recvData >> slot1 >> slot2;

    if (slot1 == slot2 || slot1 >= MAX_QUEST_LOG_SIZE || slot2 >= MAX_QUEST_LOG_SIZE)
        return;

    TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUESTLOG_SWAP_QUEST slot 1 = %u, slot 2 = %u", slot1, slot2);

    GetPlayer()->SwapQuestSlot(slot1, slot2);
}

void WorldSession::HandleQuestLogRemoveQuest(WorldPacket& recvData)
{
    uint8 slot;
    recvData >> slot;

    TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUESTLOG_REMOVE_QUEST slot = %u", slot);

    if (slot < MAX_QUEST_LOG_SIZE)
    {
        if (uint32 questId = _player->GetQuestSlotQuestId(slot))
        {
            if (!_player->TakeQuestSourceItem(questId, true))
                return;                                     // can't un-equip some items, reject quest cancel

            if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
            {
                if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED))
                    _player->RemoveTimedQuest(questId);

                if (quest->HasFlag(QUEST_FLAGS_FLAGS_PVP))
                {
                    _player->pvpInfo.IsHostile = _player->pvpInfo.IsInHostileArea || _player->HasPvPForcingQuest();
                    _player->UpdatePvPState();
                }
            }

            _player->TakeQuestSourceItem(questId, true); // remove quest src item from player
            _player->RemoveActiveQuest(questId);
            _player->RemoveTimedAchievement(ACHIEVEMENT_TIMED_TYPE_QUEST, questId);

            TC_LOG_INFO("network", "Player %u abandoned quest %u", _player->GetGUIDLow(), questId);
        }

        _player->SetQuestSlot(slot, 0);

        _player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_QUEST_ABANDONED, 1);

        if (_player)
            _player->SaveToDB(); // Save player.
    }
}

void WorldSession::HandleQuestConfirmAccept(WorldPacket& recvData)
{
    uint32 questId;
    recvData >> questId;

    TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUEST_CONFIRM_ACCEPT questId = %u", questId);

    if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
    {
        if (!quest->HasFlag(QUEST_FLAGS_PARTY_ACCEPT))
            return;

        Player* originalPlayer = ObjectAccessor::FindPlayer(_player->GetDivider());
        if (!originalPlayer)
            return;

        if (!_player->IsInSameRaidWith(originalPlayer))
            return;

        if (_player->CanAddQuest(quest, true))
            _player->AddQuestAndCheckCompletion(quest, NULL); // NULL, this prevent DB script from duplicate running

        _player->SetDivider(0);

        if (_player)
            _player->SaveToDB(); // Save player.
    }
}

void WorldSession::HandleQuestgiverCompleteQuest(WorldPacket& recvData)
{
    uint32 questId;
    uint64 playerGuid;

    bool autoCompleteMode;      // 0 - standart complete quest mode with npc, 1 - auto-complete mode

    recvData >> playerGuid >> questId >> autoCompleteMode;

    TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUESTGIVER_COMPLETE_QUEST npc = %u, questId = %u", uint32(GUID_LOPART(playerGuid)), questId);

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return;

    if (autoCompleteMode && !quest->HasFlag(QUEST_FLAGS_AUTO_COMPLETE)) // Validate AutoComplete 
        return;

    Object* object = nullptr;
    if (autoCompleteMode)
        object = _player;
    else
        object = ObjectAccessor::GetObjectByTypeMask(*_player, playerGuid, TYPEMASK_UNIT | TYPEMASK_GAMEOBJECT);

    if (!object)
        return;

   // Object* object = ObjectAccessor::GetObjectByTypeMask(*_player, playerGuid, TYPEMASK_UNIT | TYPEMASK_GAMEOBJECT);
   // if (!object || !object->hasInvolvedQuest(questId))
   //     return;

    if (autoCompleteMode == 0)
    {
        if (!object->hasInvolvedQuest(questId))
            return;

        // some kind of WPE protection
        if (!_player->CanInteractWithQuestGiver(object))
            return;
    }
    else
    {
        // Do not allow completing quests on other players.
        if (playerGuid != _player->GetGUID())
            return;
    }

    if (!_player->CanSeeStartQuest(quest) && _player->GetQuestStatus(questId) == QUEST_STATUS_NONE)
    {
        TC_LOG_ERROR("network", "Possible hacking attempt: Player %s [guid: %u] tried to complete quest [entry: %u] without being in possession of the quest!",
            _player->GetName().c_str(), _player->GetGUIDLow(), questId);
        return;
    }

    if (Battleground* bg = _player->GetBattleground())
        bg->HandleQuestComplete(questId, _player);

    if (_player->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
    {
        if (quest->IsRepeatable())
            _player->PlayerTalkClass->SendQuestGiverRequestItems(quest, playerGuid, _player->CanCompleteRepeatableQuest(quest), false);
        else
            _player->PlayerTalkClass->SendQuestGiverRequestItems(quest, playerGuid, _player->CanRewardQuest(quest, false), false);
    }
    else
    {
        if (quest->GetReqItemsCount())                  // some items required
            _player->PlayerTalkClass->SendQuestGiverRequestItems(quest, playerGuid, _player->CanRewardQuest(quest, false), false);
        else                                            // no items required
            _player->PlayerTalkClass->SendQuestGiverOfferReward(quest, playerGuid, true);
    }
}

void WorldSession::HandleQuestgiverQuestAutoLaunch(WorldPacket& /*recvPacket*/)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUESTGIVER_QUEST_AUTOLAUNCH");
}

void WorldSession::HandlePushQuestToParty(WorldPacket& recvPacket)
{
    uint32 questId;
    recvPacket >> questId;

    if (!_player->CanShareQuest(questId))
        return;

    TC_LOG_DEBUG("network", "WORLD: Received CMSG_PUSHQUESTTOPARTY questId = %u", questId);

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return;

    Player * const sender = GetPlayer();

    Group* group = sender->GetGroup();
    if (!group)
        return;

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* receiver = itr->GetSource();

        if (!receiver || receiver == sender)
            continue;

        if (!receiver->SatisfyQuestStatus(quest, false))
        {
            sender->SendPushToPartyResponse(receiver, QUEST_PARTY_MSG_HAVE_QUEST);
            continue;
        }

        if (receiver->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
        {
            sender->SendPushToPartyResponse(receiver, QUEST_PARTY_MSG_FINISH_QUEST);
            continue;
        }

        if (!receiver->CanTakeQuest(quest, false))
        {
            sender->SendPushToPartyResponse(receiver, QUEST_PARTY_MSG_CANT_TAKE_QUEST);
            continue;
        }

        if (!receiver->SatisfyQuestLog(false))
        {
            sender->SendPushToPartyResponse(receiver, QUEST_PARTY_MSG_LOG_FULL);
            continue;
        }

        if (receiver->GetDivider() != 0)
        {
            sender->SendPushToPartyResponse(receiver, QUEST_PARTY_MSG_BUSY);
            continue;
        }

        sender->SendPushToPartyResponse(receiver, QUEST_PARTY_MSG_SHARING_QUEST);

        if (quest->IsAutoAccept() && receiver->CanAddQuest(quest, true) && receiver->CanTakeQuest(quest, true))
            receiver->AddQuestAndCheckCompletion(quest, sender);

        if ((quest->IsAutoComplete() && quest->IsRepeatable() && !quest->IsDailyOrWeekly()) || quest->HasFlag(QUEST_FLAGS_AUTO_COMPLETE))
            receiver->PlayerTalkClass->SendQuestGiverRequestItems(quest, sender->GetGUID(), receiver->CanCompleteRepeatableQuest(quest), true);
        else
        {
            receiver->SetDivider(sender->GetGUID());
            receiver->PlayerTalkClass->SendQuestGiverQuestDetails(quest, sender->GetGUID(), true);
        }
    }
}

void WorldSession::HandleQuestPushResult(WorldPacket& recvPacket)
{
    uint64 guid;
    uint32 questId;
    uint8 msg;
    recvPacket >> guid >> questId >> msg;

    TC_LOG_DEBUG("network", "WORLD: Received MSG_QUEST_PUSH_RESULT");

    if (_player->GetDivider() && _player->GetDivider() == guid)
    {
        Player* player = ObjectAccessor::FindPlayer(_player->GetDivider());
        if (player)
        {
            WorldPacket data(MSG_QUEST_PUSH_RESULT, 8 + 4 + 1);
            data << uint64(_player->GetGUID());
            data << uint8(msg);                             // valid values: 0-8
            player->SendDirectMessage(&data);
            _player->SetDivider(0);
        }
    }
}

void WorldSession::HandleQuestgiverStatusMultipleQuery(WorldPacket& /*recvPacket*/)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_QUESTGIVER_STATUS_MULTIPLE_QUERY");

    uint32 count = 0;

    WorldPacket data(SMSG_QUESTGIVER_STATUS_MULTIPLE, 4 + 8 + 4);
    data << uint32(count);                                  // placeholder

    for (Player::ClientGUIDs::const_iterator itr = _player->m_clientGUIDs.begin(); itr != _player->m_clientGUIDs.end(); ++itr)
    {
        uint32 questStatus = DIALOG_STATUS_NONE;

        if (IS_CRE_OR_VEH_OR_PET_GUID(*itr))
        {
            // need also pet quests case support
            Creature* questgiver = ObjectAccessor::GetCreatureOrPetOrVehicle(*GetPlayer(), *itr);
            if (!questgiver || questgiver->IsHostileTo(_player))
                continue;
            if (!questgiver->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
                continue;

            questStatus = _player->GetQuestDialogStatus(questgiver);

            data << uint64(questgiver->GetGUID());
            data << uint32(questStatus);
            ++count;
        }
        else if (IS_GAMEOBJECT_GUID(*itr))
        {
            GameObject* questgiver = GetPlayer()->GetMap()->GetGameObject(*itr);
            if (!questgiver || questgiver->GetGoType() != GAMEOBJECT_TYPE_QUESTGIVER)
                continue;

            questStatus = _player->GetQuestDialogStatus(questgiver);

            data << uint64(questgiver->GetGUID());
            data << uint32(questStatus);
            ++count;
        }
    }

    data.put<uint32>(0, count);                             // write real count
    SendPacket(&data);
}

void WorldSession::HandleQueryQuestsCompleted(WorldPacket& /*recvData*/)
{
    size_t rew_count = _player->GetRewardedQuestCount();

    WorldPacket data(SMSG_QUERY_QUESTS_COMPLETED_RESPONSE, 4 + 4 * rew_count);
    data << uint32(rew_count);

    const RewardedQuestSet &rewQuests = _player->getRewardedQuests();
    for (RewardedQuestSet::const_iterator itr = rewQuests.begin(); itr != rewQuests.end(); ++itr)
        data << uint32(*itr);

    SendPacket(&data);
}
