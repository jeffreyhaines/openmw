//
// Created by koncord on 14.01.16.
//

#ifndef OPENMW_LOCALPLAYER_HPP
#define OPENMW_LOCALPLAYER_HPP

#include <components/openmw-mp/Base/BasePlayer.hpp>
#include <RakNetTypes.h>
#include "Networking.hpp"

namespace mwmp
{
    class LocalPlayer : public BasePlayer
    {
    public:

        LocalPlayer();
        virtual ~LocalPlayer();

        void update();

        void charGen(int stageFirst, int stageEnd);
        bool charGenThread(); // return true if CGStage::current == CGStage::end

        void updateDynamicStats(bool forceUpdate = false);
        void updateAttributes(bool forceUpdate = false);
        void updateSkills(bool forceUpdate = false);
        void updateLevel(bool forceUpdate = false);
        void updatePosition(bool forceUpdate = false);
        void updateCell(bool forceUpdate = false);
        void updateChar();
        void updateEquipped(bool forceUpdate = false);
        void updateInventory(bool forceUpdate = false);
        void updateAttackState(bool forceUpdate = false);
        void updateDeadState(bool forceUpdate = false);
        void updateDrawStateAndFlags(bool forceUpdate = false);

        void setDynamicStats();
        void setAttributes();
        void setSkills();
        void setLevel();
        void setPosition();
        void setCell();
        void setClass();
        void setInventory();

        void sendClass();
        void sendInventory();
        void sendAttack(Attack::TYPE type);

        void prepareAttack(Attack::TYPE type, bool state);

        

    private:
        Networking *getNetworking();
        MWWorld::Ptr getPlayerPtr();

    };
}

#endif //OPENMW_LOCALPLAYER_HPP