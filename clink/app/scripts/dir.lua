-- Copyright (c) 2012 Martin Ridgers
-- License: http://opensource.org/licenses/MIT

--------------------------------------------------------------------------------
clink.argmatcher("cd", "chdir", "pushd", "rd", "rmdir", "md", "mkdir"):addarg(
    function (match_word, word_index, line_state)
        local word = line_state:getword(word_index)
        local matches = {}
        for _, dir in ipairs(os.globdirs(word.."*")) do
-- TODO: PERFORMANCE: globdirs should return whether each file is hidden since
-- it already had that knowledge.
            if os.ishidden(file) then
                table.insert(matches, { match = dir, type = "dir,hidden" })
            else
                table.insert(matches, { match = dir, type = "dir" })
            end
        end
        return matches
    end
)
