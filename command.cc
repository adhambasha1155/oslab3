
/*
 * Mini shell: command table construction (parse) and execution (execute).
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <glob.h>
#include <wordexp.h>
#include <vector>
#include <string>
#include <cctype>
#include <iostream>
#include <sstream>
#include "command.h"
#include "tokenizer.h"

// ---------------------------------------------------------------------------
// Child-termination log (Part 3.4)
// ---------------------------------------------------------------------------

// Absolute path of the log file, so that `cd` does not move it.
static char g_logPath[4096] = "shell.log";

// Appends one line to the log file describing a finished child.
static void logChild(pid_t pid, int status)
{
    FILE *f = fopen(g_logPath, "a");
    if (!f)
        return;
    if (WIFEXITED(status))
        fprintf(f, "Child process %d terminated with exit status %d\n", (int)pid, WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        fprintf(f, "Child process %d killed by signal %d\n", (int)pid, WTERMSIG(status));
    fclose(f);
}

// Called when a child terminates. Reaps every finished child (so background
// jobs do not stay zombies) and logs each one.
static void sigchldHandler(int sig)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        logChild(pid, status);
}

// ---------------------------------------------------------------------------
// Parsing (Part 1)
// ---------------------------------------------------------------------------

static bool isWord(const Token &t)
{
    return t.type == TOKEN_COMMAND || t.type == TOKEN_ARGUMENT;
}

// Adds a word to the simple command. Unquoted words containing * or ? are
// expanded with glob() into one argument per matching file (bonus part).
static void addWord(SimpleCommand *sc, const std::string &word, bool quoted)
{
    if (!quoted && word.find_first_of("*?") != std::string::npos)
    {
        glob_t g;
        if (glob(word.c_str(), 0, NULL, &g) == 0)
        {
            for (size_t i = 0; i < g.gl_pathc; i++)
                sc->insertArgument(strdup(g.gl_pathv[i]));
            globfree(&g);
            return;
        }
        globfree(&g);
        // No match: keep the pattern as typed.
    }
    sc->insertArgument(strdup(word.c_str()));
}

// The tokenizer splits on spaces and keeps the quote characters, so
// "Hello, World!" arrives as two tokens: "Hello,  and  World!"
// This glues them back into one word, removes the quotes, and records in
// `quoted` which words were quoted (so they are not wildcard-expanded).
// Returns false if a quote is never closed.
static bool mergeQuotes(const std::vector<Token> &in, std::vector<Token> &out,
                        std::vector<bool> &quoted)
{
    for (size_t i = 0; i < in.size(); i++)
    {
        Token t = in[i];
        char q = t.value.empty() ? 0 : t.value[0];

        if (q != '"' && q != '\'')
        {
            out.push_back(t);
            quoted.push_back(false);
            continue;
        }

        // Opening quote: collect text until a token ends with the same quote.
        std::string text = t.value.substr(1);
        while (text.empty() || text[text.size() - 1] != q)
        {
            i++;
            if (i >= in.size() || in[i].type == TOKEN_EOF)
            {
                fprintf(stderr, "myshell: syntax error: unterminated quote\n");
                return false;
            }
            text += " " + in[i].value;
        }
        text.erase(text.size() - 1); // remove the closing quote

        t.value = text;
        out.push_back(t);
        quoted.push_back(true);
    }
    return true;
}

static void freeSimpleCommand(SimpleCommand *sc)
{
    for (int j = 0; j < sc->_numberOfArguments; j++)
        free(sc->_arguments[j]);
    free(sc->_arguments);
    delete sc;
}

// Prints a syntax error, then throws away the half-built command.
static void syntaxError(const char *msg, const char *detail, SimpleCommand *cur)
{
    fprintf(stderr, "myshell: %s '%s'\n", msg, detail);
    freeSimpleCommand(cur);
    Command::_currentCommand.clear();
}

// Text of an operator token, used in error messages.
static const char *opName(TokenType t)
{
    switch (t)
    {
    case TOKEN_REDIRECT: return ">";
    case TOKEN_APPEND: return ">>";
    case TOKEN_INPUT: return "<";
    case TOKEN_ERROR: return "2>";
    case TOKEN_REDIRECT_AND_ERROR: return ">>&";
    case TOKEN_PIPE: return "|";
    case TOKEN_BACKGROUND: return "&";
    default: return "newline";
    }
}

void parse(std::vector<Token> &rawTokens)
{
    std::vector<Token> tokens;  // rawTokens with quoted strings merged
    std::vector<bool> quoted;   // quoted[i]: tokens[i] was inside quotes
    if (!mergeQuotes(rawTokens, tokens, quoted))
        return;

    Command &cmd = Command::_currentCommand;
    SimpleCommand *cur = new SimpleCommand();

    // Blank line (only the EOF token): nothing to do.
    if (tokens.size() == 1)
    {
        freeSimpleCommand(cur);
        return;
    }

    for (size_t i = 0; i < tokens.size(); i++)
    {
        const Token &t = tokens[i];

        if (isWord(t))
        {
            addWord(cur, t.value, quoted[i]);
        }
        else if (t.type == TOKEN_PIPE)
        {
            if (cur->_numberOfArguments == 0)
            {
                syntaxError("syntax error near unexpected token", "|", cur);
                return;
            }
            if (cmd._outFile)
            {
                syntaxError("ambiguous output redirect before", "|", cur);
                return;
            }
            cmd.insertSimpleCommand(cur);
            cur = new SimpleCommand();
        }
        else if (t.type == TOKEN_REDIRECT || t.type == TOKEN_APPEND ||
                 t.type == TOKEN_INPUT || t.type == TOKEN_ERROR ||
                 t.type == TOKEN_REDIRECT_AND_ERROR)
        {
            // The next token must be the file name.
            if (i + 1 >= tokens.size() || !isWord(tokens[i + 1]))
            {
                syntaxError("syntax error: missing file name after", opName(t.type), cur);
                return;
            }
            const char *file = tokens[i + 1].value.c_str();
            i++;

            if (t.type == TOKEN_INPUT)
            {
                if (cmd._inputFile || cmd._numberOfSimpleCommands > 0)
                {
                    syntaxError("ambiguous input redirect", "<", cur);
                    return;
                }
                cmd._inputFile = strdup(file);
            }
            else if (t.type == TOKEN_ERROR)
            {
                if (cmd._errFile)
                {
                    syntaxError("ambiguous error redirect", "2>", cur);
                    return;
                }
                cmd._errFile = strdup(file);
            }
            else // >, >> or >>&
            {
                if (cmd._outFile)
                {
                    syntaxError("ambiguous output redirect", opName(t.type), cur);
                    return;
                }
                cmd._outFile = strdup(file);
                cmd._append = (t.type != TOKEN_REDIRECT);
                cmd._out_error = (t.type == TOKEN_REDIRECT_AND_ERROR);
            }
        }
        else if (t.type == TOKEN_BACKGROUND)
        {
            // & is only valid as the very last token.
            if (tokens[i + 1].type != TOKEN_EOF)
            {
                syntaxError("syntax error: '&' must be last, found", opName(tokens[i + 1].type), cur);
                return;
            }
            cmd._background = 1;
        }
        else // TOKEN_EOF
        {
            if (cur->_numberOfArguments == 0)
            {
                syntaxError("syntax error: missing command before", opName(t.type), cur);
                return;
            }
            cmd.insertSimpleCommand(cur);
        }
    }

    cmd.execute();
}

// ---------------------------------------------------------------------------
// Data structure helpers
// ---------------------------------------------------------------------------

SimpleCommand::SimpleCommand()
{
    _numberOfAvailableArguments = 5;
    _numberOfArguments = 0;
    _arguments = (char **)malloc(_numberOfAvailableArguments * sizeof(char *));
    _arguments[0] = NULL;
}

void SimpleCommand::insertArgument(char *argument)
{
    // Keep one spare slot for the NULL terminator that execvp requires.
    if (_numberOfAvailableArguments == _numberOfArguments + 1)
    {
        _numberOfAvailableArguments *= 2;
        _arguments = (char **)realloc(_arguments,
                                      _numberOfAvailableArguments * sizeof(char *));
    }

    _arguments[_numberOfArguments] = argument;
    _arguments[_numberOfArguments + 1] = NULL;

    _numberOfArguments++;
}

Command::Command()
{
    _numberOfAvailableSimpleCommands = 1;
    _numberOfSimpleCommands = 0;
    _simpleCommands = (SimpleCommand **)
        malloc(_numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));

    _outFile = 0;
    _inputFile = 0;
    _errFile = 0;
    _out_error = 0;
    _background = 0;
    _append = 0;
}

void Command::insertSimpleCommand(SimpleCommand *simpleCommand)
{
    if (_numberOfAvailableSimpleCommands == _numberOfSimpleCommands)
    {
        _numberOfAvailableSimpleCommands *= 2;
        _simpleCommands = (SimpleCommand **)realloc(_simpleCommands,
                                                    _numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));
    }

    _simpleCommands[_numberOfSimpleCommands] = simpleCommand;
    _numberOfSimpleCommands++;
}

void Command::clear()
{
    for (int i = 0; i < _numberOfSimpleCommands; i++)
        freeSimpleCommand(_simpleCommands[i]);

    if (_outFile)
    {
        free(_outFile);
    }

    if (_inputFile)
    {
        free(_inputFile);
    }

    if (_errFile)
    {
        free(_errFile);
    }

    _numberOfSimpleCommands = 0;
    _outFile = 0;
    _inputFile = 0;
    _errFile = 0;
    _out_error = 0;
    _background = 0;
    _append = 0;
}

void Command::print()
{
    printf("\n\n");
    printf("              COMMAND TABLE                \n");
    printf("\n");
    printf("  #   Simple Commands\n");
    printf("  --- ----------------------------------------------------------\n");

    for (int i = 0; i < _numberOfSimpleCommands; i++)
    {
        printf("  %-3d ", i);
        for (int j = 0; j < _simpleCommands[i]->_numberOfArguments; j++)
        {
            printf("\"%s\" \t", _simpleCommands[i]->_arguments[j]);
        }
        printf("\n");
    }

    printf("\n\n");
    printf("  Output       Input        Error        Err&Out       Background\n");
    printf("  ------------ ------------ ------------ ------------ ------------\n");
    printf("  %-12s %-12s %-12s %-12s %-12s\n", _outFile ? _outFile : "default",
           _inputFile ? _inputFile : "default", _errFile ? _errFile : "default",
           _out_error == 1 ? _outFile : "default", _background ? "YES" : "NO");
    printf("\n\n");
}

// ---------------------------------------------------------------------------
// Execution (Parts 2 and 3)
// ---------------------------------------------------------------------------

void Command::execute()
{
    print();

    // Built-ins run inside the shell process itself (no fork), because they
    // must change the shell's own state.
    if (_numberOfSimpleCommands == 1)
    {
        char **a = _simpleCommands[0]->_arguments;
        if (strcmp(a[0], "exit") == 0)
        {
            printf("\nGood bye!!\n");
            clear();
            exit(0);
        }
        if (strcmp(a[0], "cd") == 0)
        {
            const char *dir = a[1] ? a[1] : getenv("HOME");
            if (_simpleCommands[0]->_numberOfArguments > 2)
                fprintf(stderr, "myshell: cd: too many arguments\n");
            else if (dir == NULL)
                fprintf(stderr, "myshell: cd: HOME not set\n");
            else if (chdir(dir) < 0)
                fprintf(stderr, "myshell: cd: %s: %s\n", dir, strerror(errno));
            clear();
            return;
        }
    }

    // Open the redirection files in the parent. The parent's own stdin/out/err
    // are never changed, so nothing has to be saved or restored.
    int infd = -1, outfd = -1, errfd = -1;

    if (_inputFile)
    {
        infd = open(_inputFile, O_RDONLY);
        if (infd < 0)
        {
            fprintf(stderr, "myshell: %s: %s\n", _inputFile, strerror(errno));
            clear();
            return;
        }
    }
    if (_outFile)
    {
        int flags = O_WRONLY | O_CREAT | (_append ? O_APPEND : O_TRUNC);
        outfd = open(_outFile, flags, 0644);
        if (outfd < 0)
        {
            fprintf(stderr, "myshell: %s: %s\n", _outFile, strerror(errno));
            if (infd >= 0) close(infd);
            clear();
            return;
        }
    }
    if (_errFile)
    {
        errfd = open(_errFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (errfd < 0)
        {
            fprintf(stderr, "myshell: %s: %s\n", _errFile, strerror(errno));
            if (infd >= 0) close(infd);
            if (outfd >= 0) close(outfd);
            clear();
            return;
        }
    }

    // One child per simple command, chained with pipes.
    int prevRead = -1; // read end of the pipe coming from the previous command
    pid_t lastPid = -1;

    for (int i = 0; i < _numberOfSimpleCommands; i++)
    {
        bool last = (i == _numberOfSimpleCommands - 1);
        int pipefd[2] = {-1, -1};

        if (!last && pipe(pipefd) < 0)
        {
            perror("myshell: pipe");
            break;
        }

        pid_t pid = fork();
        if (pid < 0)
        {
            perror("myshell: fork");
            break;
        }

        if (pid == 0)
        {
            // Child: wire up stdin/stdout/stderr, close the originals, exec.
            signal(SIGINT, SIG_DFL); // the shell ignores Ctrl-C, its children should not

            int in = (i == 0) ? infd : prevRead;
            int out = last ? outfd : pipefd[1];
            if (in >= 0) dup2(in, 0);
            if (out >= 0) dup2(out, 1);
            if (errfd >= 0)
                dup2(errfd, 2);
            else if (last && _out_error && outfd >= 0)
                dup2(outfd, 2); // >>& sends stderr to the same file as stdout

            if (infd >= 0) close(infd);
            if (outfd >= 0) close(outfd);
            if (errfd >= 0) close(errfd);
            if (prevRead >= 0) close(prevRead);
            if (pipefd[0] >= 0) close(pipefd[0]);
            if (pipefd[1] >= 0) close(pipefd[1]);

            char **args = _simpleCommands[i]->_arguments;
            execvp(args[0], args);
            // Only reached if execvp failed.
            fprintf(stderr, "myshell: %s: %s\n", args[0], errno == ENOENT ? "command not found" : strerror(errno));
            _exit(127);
        }

        // Parent: close the pipe ends the child now owns.
        if (prevRead >= 0) close(prevRead);
        if (pipefd[1] >= 0) close(pipefd[1]);
        prevRead = pipefd[0];
        lastPid = pid;
    }

    if (prevRead >= 0) close(prevRead);
    if (infd >= 0) close(infd);
    if (outfd >= 0) close(outfd);
    if (errfd >= 0) close(errfd);

    // Foreground: wait for the last command. Background: return immediately.
    // If the SIGCHLD handler reaps the child first, waitpid fails with ECHILD
    // and the handler has already logged it, so we only log when we reaped it.
    if (!_background && lastPid > 0)
    {
        int status;
        if (waitpid(lastPid, &status, 0) == lastPid)
            logChild(lastPid, status);
    }

    clear();
}

void Command::prompt()
{
    std::string input;
    while (true)
    {
        printf("myshell>");
        fflush(stdout);
        if (!std::getline(std::cin, input))
        {
            printf("\n"); // Ctrl-D / end of input
            break;
        }
        std::vector<Token> tokens = tokenize(input);
        parse(tokens);
    }
}

Command Command::_currentCommand;
SimpleCommand *Command::_currentSimpleCommand;

int main()
{
    // Log file lives where the shell was started, whatever `cd` does later.
    if (getcwd(g_logPath, sizeof(g_logPath) - 16))
        strcat(g_logPath, "/shell.log");

    signal(SIGINT, SIG_IGN);          // Part 3.1: ignore Ctrl-C
    signal(SIGCHLD, sigchldHandler);  // Part 3.4: log every terminated child

    Command::_currentCommand.prompt();
    return 0;
}
