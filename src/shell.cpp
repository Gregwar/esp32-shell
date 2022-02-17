#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Stream *stream = nullptr;
static bool disabled = false;

#ifdef NETWORK_WIFI
#include <WiFi.h>
using NetServer = WiFiServer;
using NetClient = WiFiClient;
#endif
#ifdef NETWORK_ETHERNET
#include <Ethernet.h>
using NetServer = EthernetServer;
using NetClient = EthernetClient;
#endif

#if defined(NETWORK_WIFI) or defined(NETWORK_ETHERNET)
NetServer shell_server(3030);
NetClient shell_client;
#endif

/**
 * Global variables shell
 */

static char shell_buffer[SHELL_BUFFER_SIZE];

static bool shell_last_ok = false;
static unsigned int shell_last_pos = 0;
static unsigned int shell_pos = 0;

static const struct shell_command *shell_commands[SHELL_MAX_COMMANDS];

static unsigned int shell_command_count = 0;

static bool shell_echo_mode = true;

/**
 * Registers a command
 */
void shell_register(const struct shell_command *command) {
  shell_commands[shell_command_count++] = command;
}

static void displayHelp(bool parameter) {
  char buffer[256];
  unsigned int i;

  if (parameter) {
    shell_stream()->print("Available parameters:");
  } else {
    shell_stream()->print("Available commands:");
  }
  shell_stream()->println();

  for (i = 0; i < shell_command_count; i++) {
    const struct shell_command *command = shell_commands[i];

    if (command->parameter != parameter) {
      continue;
    }

    int namesize = strlen(command->name);
    int descsize = strlen(command->description);
    int typesize =
        (command->parameter_type == NULL) ? 0 : strlen(command->parameter_type);

    memcpy(buffer, command->name, namesize);
    buffer[namesize++] = ':';
    buffer[namesize++] = '\r';
    buffer[namesize++] = '\n';
    buffer[namesize++] = '\t';
    memcpy(buffer + namesize, command->description, descsize);
    if (typesize) {
      buffer[namesize + descsize++] = ' ';
      buffer[namesize + descsize++] = '(';
      memcpy(buffer + namesize + descsize, command->parameter_type, typesize);
      buffer[namesize + descsize + typesize++] = ')';
    }
    buffer[namesize + descsize + typesize++] = '\r';
    buffer[namesize + descsize + typesize++] = '\n';
    shell_stream()->write(buffer, namesize + descsize + typesize);
  }
}

/**
 * Internal helping command
 */
SHELL_COMMAND(help, "Displays the help about commands") { displayHelp(false); }

void shell_params_show() {
  unsigned int i;

  for (i = 0; i < shell_command_count; i++) {
    const struct shell_command *command = shell_commands[i];

    if (command->parameter) {
      command->command(0, NULL);
    }
  }
}

/**
 * Display available parameters
 */
SHELL_COMMAND(params,
              "Displays the available parameters. Usage: params [show]") {
  if (argc && strcmp(argv[0], "show") == 0) {
    shell_params_show();
  } else {
    displayHelp(true);
  }
}

/**
 * Switch echo mode
 */
SHELL_COMMAND(echo, "Switch echo mode. Usage echo [on|off]") {
  if ((argc == 1 && strcmp("on", argv[0])) || shell_echo_mode == false) {
    shell_echo_mode = true;
    shell_stream()->println("Echo enabled");
  } else {
    shell_echo_mode = false;
    shell_stream()->println("Echo disabled");
  }
}

/**
 * Write the shell prompt
 */
void shell_prompt() {
  if (shell_stream() != nullptr) {
    shell_stream()->print(SHELL_PROMPT);
  }
}

const struct shell_command *
shell_find_command(char *command_name, unsigned int command_name_length) {
  unsigned int i;

  for (i = 0; i < shell_command_count; i++) {
    const struct shell_command *command = shell_commands[i];

    if (strlen(command->name) == command_name_length &&
        strncmp(shell_buffer, command->name, command_name_length) == 0) {
      return command;
    }
  }

  return NULL;
}

/***
 * Executes the given command with given parameters
 */
bool shell_execute(char *command_name, unsigned int command_name_length,
                   unsigned int argc, char **argv) {
  unsigned int i;
  const struct shell_command *command;

  // Try to find and execute the command
  command = shell_find_command(command_name, command_name_length);
  if (command != NULL) {
    command->command(argc, argv);
  }

  // If it fails, try to parse the command as an allocation (a=b)
  if (command == NULL) {
    for (i = 0; i < command_name_length; i++) {
      if (command_name[i] == '=') {
        command_name[i] = '\0';
        command_name_length = strlen(command_name);
        command = shell_find_command(command_name, command_name_length);

        if (command && command->parameter) {
          argv[0] = command_name + i + 1;
          argv[1] = NULL;
          argc = 1;
          command->command(argc, argv);
        } else {
          command = NULL;
        }

        if (!command) {
          shell_stream()->println("Unknown parameter: ");
          stream->write(command_name, command_name_length);
          shell_stream()->println();
          return false;
        }
      }
    }
  }

  // If it fails again, display the "unknown command" message
  if (command == NULL) {
    shell_stream()->println("Unknown command: ");
    stream->write(command_name, command_name_length);
    shell_stream()->println();
    return false;
  }

  return true;
}

/***
 * Process the receive buffer to parse the command and executes it
 */
void shell_process() {
  char *saveptr;
  unsigned int command_name_length;

  unsigned int argc = 0;
  char *argv[SHELL_MAX_ARGUMENTS + 1];

  shell_stream()->println();

  strtok_r(shell_buffer, " ", &saveptr);
  while ((argv[argc] = strtok_r(NULL, " ", &saveptr)) != NULL &&
         argc < SHELL_MAX_ARGUMENTS) {
    *(argv[argc] - 1) = '\0';
    argc++;
  }

  if (saveptr != NULL) {
    *(saveptr - 1) = ' ';
  }

  command_name_length = strlen(shell_buffer);

  if (command_name_length > 0) {
    shell_last_ok =
        shell_execute(shell_buffer, command_name_length, argc, argv);
  } else {
    shell_last_ok = false;
  }

  shell_last_pos = shell_pos;
  shell_pos = 0;
  shell_prompt();
}

/**
 * Save the Serial object globaly
 */
void shell_init(uint32_t baudrate, uint32_t tcp_port) {
  Serial.begin(baudrate);
#if defined(NETWORK_WIFI) or defined(NETWORK_ETHERNET)
  shell_server.begin(tcp_port);
  stream = nullptr;
#else
  stream = &Serial;
#endif
}

TaskHandle_t shell_task_handle = NULL;

void shell_task(void*) {
  while(true) {
    shell_tick();
    vTaskDelay(10);
  }
}

void shell_start_task() {
  xTaskCreate(shell_task, "shell", 4096, NULL, 0, &shell_task_handle);
}

void shell_set_stream(Stream *stream_) { 
  stream = stream_;
}

Stream *shell_stream() { return stream; }

void shell_reset() {
  shell_pos = 0;
  shell_last_pos = 0;
  shell_buffer[0] = '\0';
  shell_last_ok = false;
  shell_prompt();
}

/**
 * Stops the shell
 */
void shell_disable() { disabled = true; }

void shell_enable() {
  shell_last_ok = false;
  disabled = false;
}

/**
 * Ticking the shell, this will cause lookup for characters
 * and eventually a call to the process function on new lines
 */
void shell_tick() {
#if defined(NETWORK_WIFI) or defined(NETWORK_ETHERNET)
  if (stream != &Serial && Serial.available()) {
    stream = &Serial;
    shell_client = NetClient();
  }

  NetClient new_client = shell_server.available();

  if (new_client || (shell_client && !shell_client.connected())) {
    shell_client = NetClient();
    if (stream == &shell_client) {
      stream = nullptr;
    }
  }

  if (new_client) {
    shell_client = new_client;
    stream = &shell_client;
  }
#endif

  if (disabled || stream == nullptr) {
    return;
  }

  char c;
  uint8_t input;

  while (stream != nullptr && stream->available()) {
    input = stream->read();
    c = (char)input;
    if (c == '\0' || c == 0xff) {
      continue;
    }

    // Return key
    if (c == '\r' || c == '\n') {
      if (shell_pos == 0 && shell_last_ok) {
        // If the user pressed no keys, restore the last
        // command and run it again
        unsigned int i;
        for (i = 0; i < shell_last_pos; i++) {
          if (shell_buffer[i] == '\0') {
            shell_buffer[i] = ' ';
          }
        }
        shell_pos = shell_last_pos;
      }
      shell_buffer[shell_pos] = '\0';
      shell_process();
      // Back key
    } else if (c == '\x7f') {
      if (shell_pos > 0) {
        shell_pos--;
        shell_stream()->print("\x8 \x8");
      }
      // Special key
    } else if (c == '\x1b') {
      stream->read();
      stream->read();
      // Others
    } else {
      shell_buffer[shell_pos] = c;
      if (shell_echo_mode) {
        shell_stream()->print(c);
      }

      if (shell_pos < SHELL_BUFFER_SIZE - 1) {
        shell_pos++;
      }
    }
  }
}
