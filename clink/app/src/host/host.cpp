// Copyright (c) 2016 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "host.h"
#include "host_lua.h"
#include "prompt.h"
#include "doskey.h"
#include "terminal/terminal.h"
#include "terminal/terminal_out.h"
#include "terminal/printer.h"
#include "utils/app_context.h"

#include <core/globber.h>
#include <core/os.h>
#include <core/path.h>
#include <core/settings.h>
#include <core/str.h>
#include <core/str_compare.h>
#include <core/str_tokeniser.h>
#include <lib/match_generator.h>
#include <lib/line_editor.h>
#include <lua/lua_script_loader.h>
#include <lua/lua_state.h>
#include <lua/lua_match_generator.h>
#include <terminal/terminal.h>
#include <readline/readline.h>

#include <list>

extern "C" {
#include <lua.h>
}

//------------------------------------------------------------------------------
static setting_enum g_ignore_case(
    "match.ignore_case",
    "Case insensitive matching",
    "Toggles whether case is ignored when selecting matches. The 'relaxed'\n"
    "option will also consider -/_ as equal.",
    "off,on,relaxed",
    2);

static setting_bool g_save_history(
    "history.save",
    "Save history between sessions",
    "Changing this setting only takes effect for new instances.",
    true);

static setting_str g_exclude_from_history_cmds(
    "history.dont_add_to_history_cmds",
    "Commands not automatically added to the history",
    "List of commands that aren't automatically added to the history.\n"
    "Commands are separated by spaces, commas, or semicolons.  Default is\n"
    "\"exit history\", to exclude both of those commands.",
    "exit history");



//------------------------------------------------------------------------------
class dir_history_entry : public no_copy
{
public:
                    dir_history_entry(const char* s);
                    dir_history_entry(dir_history_entry&& d) { dir = d.dir; d.dir = nullptr; }
                    ~dir_history_entry() { free(dir); }

    const char*     get() const { return dir; }

private:
    char* dir;
};

//------------------------------------------------------------------------------
dir_history_entry::dir_history_entry(const char* s)
{
    size_t alloc = strlen(s) + 1;
    dir = (char *)malloc(alloc);
    memcpy(dir, s, alloc);
}

//------------------------------------------------------------------------------
const int c_max_dir_history = 100;
static std::list<dir_history_entry> s_dir_history;

//------------------------------------------------------------------------------
static void update_dir_history()
{
    str<> cwd;
    os::get_current_dir(cwd);

    // Add cwd to tail.
    if (!s_dir_history.size() || _stricmp(s_dir_history.back().get(), cwd.c_str()) != 0)
        s_dir_history.push_back(cwd.c_str());

    // Trim overflow from head.
    while (s_dir_history.size() > c_max_dir_history)
        s_dir_history.pop_front();
}

//------------------------------------------------------------------------------
static void prev_dir_history(str_base& inout)
{
    inout.clear();

    if (s_dir_history.size() < 2)
        return;

    auto a = s_dir_history.rbegin();
    a++;

    inout.format(" cd /d \"%s\"", a->get());
}

//------------------------------------------------------------------------------
const char** host_copy_dir_history(int* total)
{
    if (!s_dir_history.size())
        return nullptr;

    // Copy the directory list (just a shallow copy of the dir pointers).
    const char** history = (const char**)malloc(sizeof(*history) * s_dir_history.size());
    int i = 0;
    for (auto const& it : s_dir_history)
        history[i++] = it.get();

    *total = i;
    return history;
}



//------------------------------------------------------------------------------
static history_db* s_history_db = nullptr;
void host_add_history(int, const char* line)
{
    if (s_history_db)
        s_history_db->add(line);
}
void host_remove_history(int rl_history_index, const char* line)
{
    if (s_history_db)
        s_history_db->remove(rl_history_index, line);
}

//------------------------------------------------------------------------------
static void write_line_feed()
{
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    WriteConsoleW(handle, L"\n", 1, &written, nullptr);
}

//------------------------------------------------------------------------------
static bool intercept_directory(str_base& inout)
{
    const char* line = inout.c_str();

    // Check for '-' (etc) to change to previous directory.
    if (strcmp(line, "-") == 0 ||
        _strcmpi(line, "cd -") == 0 ||
        _strcmpi(line, "chdir -") == 0)
    {
        prev_dir_history(inout);
        return true;
    }

    // Skip leading whitespace.
    while (*line == ' ' || *line == '\t')
        line++;

    // Strip all quotes (it may be surprising, but this is what CMD.EXE does).
    str<> tmp;
    while (*line)
    {
        if (*line != '\"')
            tmp.concat(line, 1);
        line++;
    }

    // Truncate trailing whitespcae.
    while (tmp.length())
    {
        char ch = tmp.c_str()[tmp.length() - 1];
        if (ch != ' ' && ch != '\t')
            break;
        tmp.truncate(tmp.length() - 1);
    }

    if (!tmp.length())
        return false;

    // If all dots, convert into valid path syntax moving N-1 levels.
    // Examples:
    //  - "..." becomes "..\..\"
    //  - "...." becomes "..\..\..\"
    int num_dots = 0;
    for (const char* p = tmp.c_str(); *p; ++p, ++num_dots)
    {
        if (*p != '.')
        {
            num_dots = -1;
            break;
        }
    }
    if (num_dots >= 3)
    {
        tmp.clear();
        while (num_dots > 1)
        {
            tmp.concat("..\\");
            --num_dots;
        }
    }

    // If the input doesn't end with a separator, don't handle it.  Otherwise
    // it would interfere with launching something found on the PATH but with
    // the same name as a subdirectory of the current working directory.
    if (!path::is_separator(tmp.c_str()[tmp.length() - 1]))
    {
        // But allow a special case for "..\.." and "..\..\..", etc.
        const char* p = tmp.c_str();
        while (true)
        {
            if (p[0] != '.' || p[1] != '.')
                return false;
            if (p[2] == '\0')
                break;
            if (!path::is_separator(p[2]))
                return false;
            p += 3;
        }
    }

    if (os::get_path_type(tmp.c_str()) != os::path_type_dir)
        return false;

    inout.format(" cd /d \"%s\"", tmp.c_str());
    return true;
}



//------------------------------------------------------------------------------
host::host(const char* name)
: m_name(name)
, m_doskey("cmd.exe")
{
    m_terminal = terminal_create();
    m_printer = new printer(*m_terminal.out);
}

//------------------------------------------------------------------------------
host::~host()
{
    delete m_history;
    delete m_printer;
    terminal_destroy(m_terminal);
}

//------------------------------------------------------------------------------
bool host::edit_line(const char* prompt, str_base& out)
{
    const app_context* app = app_context::get();
    app->update_env();

    struct cwd_restorer
    {
        cwd_restorer()  { os::get_current_dir(m_path); }
        ~cwd_restorer() { os::set_current_dir(m_path.c_str()); }
        str<288>        m_path;
    } cwd;

    // Load Clink's settings.
    str<288> settings_file;
    app_context::get()->get_settings_path(settings_file);
    settings::load(settings_file.c_str());

    // Set up the string comparison mode.
    int cmp_mode;
    switch (g_ignore_case.get())
    {
    case 1:     cmp_mode = str_compare_scope::caseless; break;
    case 2:     cmp_mode = str_compare_scope::relaxed;  break;
    default:    cmp_mode = str_compare_scope::exact;    break;
    }
    str_compare_scope compare(cmp_mode);

    // Set up Lua and load scripts into it.
    str<288> script_path;
    app_context::get()->get_script_path(script_path);
    host_lua lua(script_path.c_str());
    prompt_filter prompt_filter(lua);
    initialise_lua(lua);
    lua.load_scripts();

    // Unfortunately we need to load settings again because some settings don't
    // exist until after Lua's up and running. But.. we can't load Lua scripts
    // without loading settings first. [TODO: find a better way]
    settings::load(settings_file.c_str());

    line_editor::desc desc = {};
    initialise_editor_desc(desc);

    // Filter the prompt.
    str<256> filtered_prompt;
    prompt_filter.filter(prompt, filtered_prompt);
    desc.prompt = filtered_prompt.c_str();

    // Set the terminal that will handle all IO while editing.
    desc.input = m_terminal.in;
    desc.output = m_terminal.out;
    desc.printer = m_printer;

    // Create the editor and add components to it.
    line_editor* editor = line_editor_create(desc);

    editor->add_generator(lua);
    editor->add_generator(file_match_generator());

    if (g_save_history.get())
    {
        if (!m_history)
            m_history = new history_db;
    }
    else
    {
        if (m_history)
        {
            delete m_history;
            m_history = nullptr;
        }
    }

    if (m_history)
    {
        m_history->initialise();
        m_history->load_rl_history();
    }

    s_history_db = m_history;

    bool resolved = false;
    bool ret = false;
    while (1)
    {
        // Give the directory history queue a crack at the current directory.
        update_dir_history();

        // Doskey is implemented on the server side of a ReadConsoleW() call
        // (i.e. in conhost.exe). Commands separated by a "$T" are returned one
        // command at a time through successive calls to ReadConsoleW().
        resolved = false;
        if (m_doskey_alias.next(out))
        {
            m_terminal.out->begin();
            m_terminal.out->write(filtered_prompt.c_str(), filtered_prompt.length());
            m_terminal.out->write(out.c_str(), out.length());
            m_terminal.out->end();
            write_line_feed();
            resolved = true;
            ret = true;
        }
        else if (ret = editor->edit(out))
        {
            // Handle history event expansion.  expand() is a static method,
            // so can call it even when m_history is nullptr.
            if (m_history->expand(out.c_str(), out) == history_db::expand_print)
            {
                puts(out.c_str());
                continue;
            }

            // Should we skip adding certain commands?
            if (g_exclude_from_history_cmds.get() &&
                *g_exclude_from_history_cmds.get())
            {
                const char* c = out.c_str();
                while (*c == ' ' || *c == '\t')
                    ++c;

                bool exclude = false;
                str<> token;
                str_tokeniser tokens(g_exclude_from_history_cmds.get(), " ,;");
                while (tokens.next(token))
                {
                    if (token.length() &&
                        _strnicmp(c, token.c_str(), token.length()) == 0 &&
                        !isalnum((unsigned char)c[token.length()]) &&
                        !path::is_separator(c[token.length()]))
                    {
                        exclude = true;
                        break;
                    }
                }

                if (exclude)
                    break;
            }

            // Add the line to the history.
            if (m_history)
                m_history->add(out.c_str());
        }

        if (ret)
        {
            // If the line is a directory, rewrite the line to invoke the CD
            // command to change to the directory.
            intercept_directory(out);
        }
        break;
    }

    if (!resolved)
    {
        m_doskey.resolve(out.c_str(), m_doskey_alias);
        m_doskey_alias.next(out);
    }

    s_history_db = nullptr;

    line_editor_destroy(editor);
    return ret;
}
