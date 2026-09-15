//
// Vita "game bubble" generator: takes the currently selected rom and
// installs it as its own standalone LiveArea bubble (own icon, own title,
// launches straight into the game, no rom list).
//

#ifndef PEMU_BUBBLE_MAKER_H
#define PEMU_BUBBLE_MAKER_H

#include <string>
#include "ss_game.h"

namespace pemu {

    class UiMain;

    class BubbleMaker {
    public:
        // returns empty string on success, or an error message on failure
        static std::string createBubble(UiMain *ui, const ss_api::Game &game);

    private:
        static std::string makeTitleId(const std::string &name);

        static bool patchSfo(std::vector<char> &sfo, const std::string &titleId,
                             const std::string &title);
    };
}

#endif //PEMU_BUBBLE_MAKER_H
