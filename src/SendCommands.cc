#include "SendCommands.hh"

#include <inttypes.h>
#include <string.h>
#include <event2/buffer.h>

#include <memory>
#include <phosg/Encoding.hh>
#include <phosg/Random.hh>
#include <phosg/Strings.hh>
#include <phosg/Time.hh>

#include "PSOProtocol.hh"
#include "CommandFormats.hh"
#include "FileContentsCache.hh"
#include "Text.hh"

using namespace std;



static FileContentsCache file_cache;



void send_command(
    struct bufferevent* bev,
    GameVersion version,
    PSOEncryption* crypt,
    uint16_t command,
    uint32_t flag,
    const void* data,
    size_t size,
    const char* name_str) {
  string send_data;

  switch (version) {
    case GameVersion::GC:
    case GameVersion::DC: {
      PSOCommandHeaderDCGC header;
      header.command = command;
      header.flag = flag;
      header.size = sizeof(header) + size;
      send_data.append(reinterpret_cast<const char*>(&header), sizeof(header));
      if (size) {
        send_data.append(reinterpret_cast<const char*>(data), size);
        send_data.resize((send_data.size() + 3) & ~3);
      }
      break;
    }

    case GameVersion::PC:
    case GameVersion::PATCH: {
      PSOCommandHeaderPC header;
      header.size = sizeof(header) + size;
      header.command = command;
      header.flag = flag;
      send_data.append(reinterpret_cast<const char*>(&header), sizeof(header));
      if (size) {
        send_data.append(reinterpret_cast<const char*>(data), size);
        send_data.resize((send_data.size() + 3) & ~3);
      }
      break;
    }

    case GameVersion::BB: {
      PSOCommandHeaderBB header;
      header.size = sizeof(header) + size;
      header.command = command;
      header.flag = flag;
      send_data.append(reinterpret_cast<const char*>(&header), sizeof(header));
      if (size) {
        send_data.append(reinterpret_cast<const char*>(data), size);
        send_data.resize((send_data.size() + 7) & ~7);
      }
      break;
    }

    default:
      throw logic_error("unimplemented game version in send_command");
  }

  if (name_str) {
    string name_token;
    if (name_str[0]) {
      name_token = string(" to ") + name_str;
    }
    log(INFO, "Sending%s (version=%d command=%04hX flag=%08X)",
        name_token.c_str(), static_cast<int>(version), command, flag);
    print_data(stderr, send_data.data(), send_data.size());
  }

  if (crypt) {
    crypt->encrypt(send_data.data(), send_data.size());
  }

  struct evbuffer* buf = bufferevent_get_output(bev);
  evbuffer_add(buf, send_data.data(), send_data.size());
}

void send_command(shared_ptr<Client> c, uint16_t command, uint32_t flag,
    const void* data, size_t size) {
  if (!c->bev) {
    return;
  }
  string encoded_name = remove_language_marker(encode_sjis(c->player.disp.name));
  send_command(c->bev, c->version, c->crypt_out.get(), command, flag, data,
      size, encoded_name.c_str());
}

void send_command_excluding_client(shared_ptr<Lobby> l, shared_ptr<Client> c,
    uint16_t command, uint32_t flag, const void* data, size_t size) {
  for (auto& client : l->clients) {
    if (!client || (client == c)) {
      continue;
    }
    send_command(client, command, flag, data, size);
  }
}

void send_command(shared_ptr<Lobby> l, uint16_t command, uint32_t flag,
    const void* data, size_t size) {
  send_command_excluding_client(l, nullptr, command, flag, data, size);
}

void send_command(shared_ptr<ServerState> s, uint16_t command, uint32_t flag,
    const void* data, size_t size) {
  for (auto& l : s->all_lobbies()) {
    send_command(l, command, flag, data, size);
  }
}



// specific command sending functions follow. in general, they're written in
// such a way that you don't need to think about anything, even the client's
// version, before calling them. for this reason, some of them are quite
// complex. many are split into several functions, one for each version of PSO,
// named with suffixes _GC, _BB, and the like. in these cases, the function
// without the suffix simply calls the appropriate function for the client's
// version. thus, if you change something in one of the version-specific
// functions, you may have to change it in all of them.

////////////////////////////////////////////////////////////////////////////////
// CommandServerInit: this function sends the command that initializes encryption

// strings needed for various functions
static const char* anti_copyright = "This server is in no way affiliated, sponsored, or supported by SEGA Enterprises or SONICTEAM. The preceding message exists only in order to remain compatible with programs that expect it.";
static const char* dc_port_map_copyright = "DreamCast Port Map. Copyright SEGA Enterprises. 1999";
static const char* dc_lobby_server_copyright = "DreamCast Lobby Server. Copyright SEGA Enterprises. 1999";
static const char* bb_game_server_copyright = "Phantasy Star Online Blue Burst Game Server. Copyright 1999-2004 SONICTEAM.";
static const char* patch_server_copyright = "Patch Server. Copyright SonicTeam, LTD. 2001";

S_ServerInit_DC_GC_02_17 prepare_server_init_contents_dc_pc_gc(
    bool initial_connection,
    uint32_t server_key,
    uint32_t client_key) {
  S_ServerInit_DC_GC_02_17 cmd;
  cmd.copyright = initial_connection
      ? dc_port_map_copyright : dc_lobby_server_copyright;
  cmd.server_key = server_key;
  cmd.client_key = client_key;
  cmd.after_message = anti_copyright;
  return cmd;
}

void send_server_init_dc_pc_gc(shared_ptr<Client> c,
    bool initial_connection) {
  // PC uses 17 for all server inits; GC uses it only for the first one
  uint8_t command = (initial_connection || (c->version == GameVersion::PC))
      ? 0x17 : 0x02;
  uint32_t server_key = random_object<uint32_t>();
  uint32_t client_key = random_object<uint32_t>();

  auto cmd = prepare_server_init_contents_dc_pc_gc(
      initial_connection, server_key, client_key);
  send_command(c, command, 0x00, cmd);

  switch (c->version) {
    case GameVersion::DC:
    case GameVersion::PC:
      c->crypt_out.reset(new PSOPCEncryption(server_key));
      c->crypt_in.reset(new PSOPCEncryption(client_key));
      break;
    case GameVersion::GC:
      c->crypt_out.reset(new PSOGCEncryption(server_key));
      c->crypt_in.reset(new PSOGCEncryption(client_key));
      break;
    default:
      throw invalid_argument("incorrect client version");
  }
}

void send_server_init_bb(shared_ptr<ServerState> s, shared_ptr<Client> c) {
  S_ServerInit_BB_03 cmd;
  cmd.copyright = bb_game_server_copyright;
  random_data(cmd.server_key.data(), cmd.server_key.bytes());
  random_data(cmd.client_key.data(), cmd.client_key.bytes());
  cmd.after_message = anti_copyright;
  send_command(c, 0x03, 0x00, cmd);

  c->crypt_out.reset(new PSOBBEncryption(s->default_key_file,
      cmd.server_key.data(), cmd.server_key.bytes()));
  c->crypt_in.reset(new PSOBBEncryption(s->default_key_file,
      cmd.client_key.data(), cmd.client_key.bytes()));
}

void send_server_init_patch(shared_ptr<Client> c) {
  uint32_t server_key = random_object<uint32_t>();
  uint32_t client_key = random_object<uint32_t>();

  S_ServerInit_Patch_02 cmd;
  cmd.copyright = patch_server_copyright;
  cmd.server_key = server_key;
  cmd.client_key = client_key;
  send_command(c, 0x02, 0x00, cmd);

  c->crypt_out.reset(new PSOPCEncryption(server_key));
  c->crypt_in.reset(new PSOPCEncryption(client_key));
}

void send_server_init(shared_ptr<ServerState> s, shared_ptr<Client> c,
    bool initial_connection) {
  switch (c->version) {
    case GameVersion::DC:
    case GameVersion::PC:
    case GameVersion::GC:
      send_server_init_dc_pc_gc(c, initial_connection);
      break;
    case GameVersion::PATCH:
      send_server_init_patch(c);
      break;
    case GameVersion::BB:
      send_server_init_bb(s, c);
      break;
    default:
      throw logic_error("unimplemented versioned command");
  }
}



// for non-BB clients, updates the client's guild card and security data
void send_update_client_config(shared_ptr<Client> c) {
  S_UpdateClientConfig_DC_PC_GC_04 cmd;
  cmd.player_tag = 0x00010000;
  cmd.guild_card_number = c->license->serial_number;
  cmd.cfg = c->export_config();
  send_command(c, 0x04, 0x00, cmd);
}



void send_reconnect(shared_ptr<Client> c, uint32_t address, uint16_t port) {
  S_Reconnect_19 cmd = {address, port, 0};
  send_command(c, 0x19, 0x00, cmd);
}

// sends the command (first used by Schthack) that separates PC and GC users
// that connect on the same port
void send_pc_gc_split_reconnect(shared_ptr<Client> c, uint32_t address,
    uint16_t pc_port, uint16_t gc_port) {
  S_ReconnectSplit_19 cmd;
  cmd.pc_address = address;
  cmd.pc_port = pc_port;
  cmd.gc_command = 0x19;
  cmd.gc_size = 0x97;
  cmd.gc_address = address;
  cmd.gc_port = gc_port;
  send_command(c, 0x19, 0x00, cmd);
}



void send_client_init_bb(shared_ptr<Client> c, uint32_t error) {
  S_ClientInit_BB_E6 cmd;
  cmd.error = error;
  cmd.player_tag = 0x00010000;
  cmd.guild_card_number = c->license->serial_number;
  cmd.team_id = static_cast<uint32_t>(random_object<uint32_t>());
  cmd.cfg = c->export_config_bb();
  cmd.caps = 0x00000102;
  send_command(c, 0x00E6, 0x00000000, cmd);
}

void send_team_and_key_config_bb(shared_ptr<Client> c) {
  send_command(c, 0x00E2, 0x00000000, c->player.key_config);
}

void send_player_preview_bb(shared_ptr<Client> c, uint8_t player_index,
    const PlayerDispDataBBPreview* preview) {

  if (!preview) {
    // no player exists
    S_PlayerPreview_NoPlayer_BB_E4 cmd = {player_index, 0x00000002};
    send_command(c, 0x00E4, 0x00000000, cmd);

  } else {
    S_PlayerPreview_BB_E3 cmd = {player_index, *preview};
    send_command(c, 0x00E3, 0x00000000, cmd);
  }
}

void send_accept_client_checksum_bb(shared_ptr<Client> c) {
  S_AcceptClientChecksum_BB_02E8 cmd = {1, 0};
  send_command(c, 0x02E8, 0x00000000, cmd);
}

void send_guild_card_header_bb(shared_ptr<Client> c) {
  uint32_t checksum = compute_guild_card_checksum(&c->player.guild_cards,
      sizeof(GuildCardFileBB));
  S_GuildCardHeader_BB_01DC cmd = {1, 0x00000490, checksum};
  send_command(c, 0x01DC, 0x00000000, cmd);
}

void send_guild_card_chunk_bb(shared_ptr<Client> c, size_t chunk_index) {
  size_t chunk_offset = chunk_index * 0x6800;
  if (chunk_offset >= sizeof(GuildCardFileBB)) {
    throw logic_error("attempted to send chunk beyond end of guild card file");
  }
  size_t data_size = sizeof(GuildCardFileBB) - chunk_offset;
  if (data_size > 0x6800) {
    data_size = 0x6800;
  }

  StringWriter w;
  w.put_u32l(0);
  w.put_u32l(chunk_index);
  w.write(&c->player.guild_cards + chunk_offset, data_size);

  send_command(c, 0x02DC, 0x00000000, w.str());
}

void send_stream_file_bb(shared_ptr<Client> c) {
  auto index_data = file_cache.get("system/blueburst/streamfile.ind");
  if (index_data->size() % sizeof(S_StreamFileIndexEntry_BB_01EB)) {
    throw invalid_argument("stream file index not a multiple of entry size");
  }

  size_t entry_count = index_data->size() / sizeof(S_StreamFileIndexEntry_BB_01EB);
  send_command(c, 0x01EB, entry_count, index_data);

  auto* entries = reinterpret_cast<const S_StreamFileIndexEntry_BB_01EB*>(index_data->data());

  S_StreamFileChunk_BB_02EB chunk_cmd;
  chunk_cmd.chunk_index = 0;

  uint32_t buffer_offset = 0;
  for (size_t x = 0; x < entry_count; x++) {
    auto filename = string_printf("system/blueburst/%s", entries[x].filename.c_str());
    auto file_data = file_cache.get(filename);

    size_t file_data_remaining = file_data->size();
    if (file_data_remaining != entries[x].size) {
      throw invalid_argument(filename + " does not match size in stream file index");
    }
    while (file_data_remaining) {
      size_t read_size = 0x6800 - buffer_offset;
      if (read_size > file_data_remaining) {
        read_size = file_data_remaining;
      }
      memcpy(&chunk_cmd.data[buffer_offset],
          file_data->data() + file_data->size() - file_data_remaining, read_size);
      // TODO: We probably should clear the rest of the buffer on the last chunk
      buffer_offset += read_size;
      file_data_remaining -= read_size;

      if (buffer_offset == 0x6800) {
        // Note: the client sends 0x03EB in response to these, but we'll just
        // ignore them because we don't need any of the contents
        send_command(c, 0x02EB, 0x00000000, chunk_cmd);
        buffer_offset = 0;
        chunk_cmd.chunk_index++;
      }
    }

    if (buffer_offset > 0) {
      send_command(c, 0x02EB, 0x00000000, &chunk_cmd, (buffer_offset + 15) & ~3);
    }
  }
}

void send_approve_player_choice_bb(shared_ptr<Client> c) {
  S_ApprovePlayerChoice_BB_00E4 cmd = {c->bb_player_index, 1};
  send_command(c, 0x00E4, 0x00000000, cmd);
}

void send_complete_player_bb(shared_ptr<Client> c) {
  send_command(c, 0x00E7, 0x00000000, c->player.export_bb_player_data());
}



////////////////////////////////////////////////////////////////////////////////
// patch functions

void send_check_directory_patch(shared_ptr<Client> c, const char* dir) {
  S_CheckDirectory_Patch_09 cmd = {dir};
  send_command(c, 0x09, 0x00, cmd);
}



////////////////////////////////////////////////////////////////////////////////
// message functions

void send_text(shared_ptr<Client> c, StringWriter& w, uint16_t command,
    const char16_t* text) {
  if ((c->version == GameVersion::DC) || (c->version == GameVersion::GC)) {
    string data = encode_sjis(text);
    add_color(w, data.c_str(), data.size());
  } else {
    add_color(w, text, text_strlen_t(text));
  }
  while (w.str().size() & 3) {
    w.put_u8(0);
  }
  send_command(c, command, 0x00, w.str());
}

void send_header_text(shared_ptr<Client> c, uint16_t command,
    uint32_t guild_card_number, const char16_t* text) {
  StringWriter w;
  w.put(SC_TextHeader_01_06_11({0, guild_card_number}));
  send_text(c, w, command, text);
}

void send_text(shared_ptr<Client> c, uint16_t command,
    const char16_t* text) {
  StringWriter w;
  send_text(c, w, command, text);
}

void send_message_box(shared_ptr<Client> c, const char16_t* text) {
  uint16_t command = (c->version == GameVersion::PATCH) ? 0x13 : 0x1A;
  send_text(c, command, text);
}

void send_lobby_name(shared_ptr<Client> c, const char16_t* text) {
  send_text(c, 0x8A, text);
}

void send_quest_info(shared_ptr<Client> c, const char16_t* text) {
  send_text(c, 0xA3, text);
}

void send_lobby_message_box(shared_ptr<Client> c, const char16_t* text) {
  send_header_text(c, 0x01, 0, text);
}

void send_ship_info(shared_ptr<Client> c, const char16_t* text) {
  send_header_text(c, 0x11, 0, text);
}

void send_text_message(shared_ptr<Client> c, const char16_t* text) {
  send_header_text(c, 0xB0, 0, text);
}

void send_text_message(shared_ptr<Lobby> l, const char16_t* text) {
  for (size_t x = 0; x < l->max_clients; x++) {
    if (l->clients[x]) {
      send_text_message(l->clients[x], text);
    }
  }
}

void send_text_message(shared_ptr<ServerState> s, const char16_t* text) {
  // TODO: We should have a collection of all clients (even those not in any
  // lobby) and use that instead here
  for (auto& l : s->all_lobbies()) {
    send_text_message(l, text);
  }
}

void send_chat_message(shared_ptr<Client> c, uint32_t from_serial_number,
    const char16_t* from_name, const char16_t* text) {
  u16string data;
  if (c->version == GameVersion::BB) {
    data.append(u"\x09J");
  }
  data.append(remove_language_marker(from_name));
  data.append(u"\x09\x09J");
  data.append(text);
  send_header_text(c, 0x06, from_serial_number, data.c_str());
}

void send_simple_mail_gc(std::shared_ptr<Client> c, uint32_t from_serial_number,
    const char16_t* from_name, const char16_t* text) {
  SC_SimpleMail_GC_81 cmd;
  cmd.player_tag = 0x00010000;
  cmd.from_serial_number = from_serial_number;
  cmd.from_name = from_name;
  cmd.to_serial_number = c->license->serial_number;
  cmd.text = text;
  send_command(c, 0x81, 0x00, cmd);
}

void send_simple_mail(std::shared_ptr<Client> c, uint32_t from_serial_number,
    const char16_t* from_name, const char16_t* text) {
  if (c->version == GameVersion::GC) {
    send_simple_mail_gc(c, from_serial_number, from_name, text);
  } else {
    throw logic_error("unimplemented versioned command");
  }
}



////////////////////////////////////////////////////////////////////////////////
// info board

template <typename CharT>
void send_info_board_t(shared_ptr<Client> c, shared_ptr<Lobby> l) {
  vector<S_InfoBoardEntry_D8<CharT>> entries;
  for (const auto& c : l->clients) {
    if (!c.get()) {
      continue;
    }
    auto& e = entries.emplace_back();
    e.name = c->player.disp.name;
    e.message = c->player.info_board;
    add_color_inplace(e.message);
  }
  send_command(c, 0xD8, entries.size(), entries);
}

void send_info_board(shared_ptr<Client> c, shared_ptr<Lobby> l) {
  if (c->version == GameVersion::PC || c->version == GameVersion::PATCH ||
      c->version == GameVersion::BB) {
    send_info_board_t<char16_t>(c, l);
  } else {
    send_info_board_t<char>(c, l);
  }
}



////////////////////////////////////////////////////////////////////////////////
// CommandCardSearchResult: sends a guild card search result to a player.

template <typename CommandHeaderT, typename CharT>
void send_card_search_result_t(
    shared_ptr<ServerState> s,
    shared_ptr<Client> c,
    shared_ptr<Client> result,
    shared_ptr<Lobby> result_lobby) {
  S_GuildCardSearchResult<CommandHeaderT, CharT> cmd;
  cmd.player_tag = 0x00010000;
  cmd.searcher_serial_number = c->license->serial_number;
  cmd.result_serial_number = result->license->serial_number;
  cmd.reconnect_command_header.size = sizeof(cmd.reconnect_command_header) + sizeof(cmd.reconnect_command);
  cmd.reconnect_command_header.command = 0x19;
  cmd.reconnect_command_header.flag = 0x00;
  // TODO: make this actually make sense... currently we just take the sockname
  // for the target client. This also doesn't work if the client is on a virtual
  // connection (the address and port are zero).
  const sockaddr_in* local_addr = reinterpret_cast<const sockaddr_in*>(&result->local_addr);
  cmd.reconnect_command.address = local_addr->sin_addr.s_addr;
  cmd.reconnect_command.port = ntohs(local_addr->sin_port);
  cmd.reconnect_command.unused = 0;

  auto encoded_server_name = encode_sjis(s->name);
  string location_string;
  if (result_lobby->is_game()) {
    string encoded_lobby_name = encode_sjis(result_lobby->name);
    location_string = string_printf("%s,Block 00,,%s",
        encoded_lobby_name.c_str(), encoded_server_name.c_str());
  } else {
    location_string = string_printf("Block 00,,%s", encoded_server_name.c_str());
  }
  cmd.location_string = location_string;
  cmd.menu_id = LOBBY_MENU_ID;
  cmd.lobby_id = result->lobby_id;
  cmd.name = result->player.disp.name;

  send_command(c, 0x41, 0x00, cmd);
}

void send_card_search_result(
    shared_ptr<ServerState> s,
    shared_ptr<Client> c,
    shared_ptr<Client> result,
    shared_ptr<Lobby> result_lobby) {
  if ((c->version == GameVersion::DC) || (c->version == GameVersion::GC)) {
    send_card_search_result_t<PSOCommandHeaderDCGC, char>(
        s, c, result, result_lobby);
  } else if (c->version == GameVersion::PC) {
    send_card_search_result_t<PSOCommandHeaderPC, char16_t>(
        s, c, result, result_lobby);
  } else if (c->version == GameVersion::BB) {
    send_card_search_result_t<PSOCommandHeaderBB, char16_t>(
        s, c, result, result_lobby);
  } else {
    throw logic_error("unimplemented versioned command");
  }
}



void send_guild_card_gc(shared_ptr<Client> c, shared_ptr<Client> source) {
  S_SendGuildCard_GC cmd;
  cmd.subcommand = 0x06;
  cmd.subsize = 0x25;
  cmd.unused = 0x0000;
  cmd.player_tag = 0x00010000;
  cmd.reserved1 = 1;
  cmd.reserved2 = 1;
  cmd.serial_number = source->license->serial_number;
  cmd.name = source->player.disp.name;
  remove_language_marker_inplace(cmd.name);
  cmd.desc = source->player.guild_card_desc;
  cmd.section_id = source->player.disp.section_id;
  cmd.char_class = source->player.disp.char_class;
  send_command(c, 0x62, c->lobby_client_id, cmd);
}

void send_guild_card_bb(shared_ptr<Client> c, shared_ptr<Client> source) {
  S_SendGuildCard_BB cmd;
  cmd.subcommand = 0x06;
  cmd.subsize = 0x43;
  cmd.unused = 0x0000;
  cmd.reserved1 = 1;
  cmd.reserved2 = 1;
  cmd.serial_number = source->license->serial_number;
  cmd.name = remove_language_marker(source->player.disp.name);
  cmd.team_name = remove_language_marker(source->player.team_name);
  cmd.desc = source->player.guild_card_desc;
  cmd.section_id = source->player.disp.section_id;
  cmd.char_class = source->player.disp.char_class;
  send_command(c, 0x62, c->lobby_client_id, cmd);
}

void send_guild_card(shared_ptr<Client> c, shared_ptr<Client> source) {
  if (c->version == GameVersion::GC) {
    send_guild_card_gc(c, source);
  } else if (c->version == GameVersion::BB) {
    send_guild_card_bb(c, source);
  } else {
    throw logic_error("unimplemented versioned command");
  }
}



////////////////////////////////////////////////////////////////////////////////
// menus

template <typename EntryT>
void send_menu_t(
    shared_ptr<Client> c,
    const char16_t* menu_name,
    uint32_t menu_id,
    const vector<MenuItem>& items,
    bool is_info_menu) {

  vector<EntryT> entries;
  {
    auto& e = entries.emplace_back();
    e.menu_id = menu_id;
    e.item_id = 0xFFFFFFFF;
    e.flags = 0x0004;
    e.text = menu_name;
  }

  for (const auto& item : items) {
    if (((c->version == GameVersion::DC) && (item.flags & MenuItemFlag::INVISIBLE_ON_DC)) ||
        ((c->version == GameVersion::PC) && (item.flags & MenuItemFlag::INVISIBLE_ON_PC)) ||
        ((c->version == GameVersion::GC) && (item.flags & MenuItemFlag::INVISIBLE_ON_GC)) ||
        ((c->version == GameVersion::BB) && (item.flags & MenuItemFlag::INVISIBLE_ON_BB)) ||
        ((c->flags & ClientFlag::EPISODE_3_GAMES) && (item.flags & MenuItemFlag::INVISIBLE_ON_GC_EPISODE_3)) ||
        ((item.flags & MenuItemFlag::REQUIRES_MESSAGE_BOXES) && (c->flags & ClientFlag::NO_MESSAGE_BOX_CLOSE_CONFIRMATION))) {
      continue;
    }
    auto& e = entries.emplace_back();
    e.menu_id = menu_id;
    e.item_id = item.item_id;
    e.flags = (c->version == GameVersion::BB) ? 0x0004 : 0x0F04;
    e.text = item.name;
  }

  send_command(c, is_info_menu ? 0x1F : 0x07, entries.size() - 1, entries);
}

void send_menu(shared_ptr<Client> c, const char16_t* menu_name,
    uint32_t menu_id, const vector<MenuItem>& items, bool is_info_menu) {
  if (c->version == GameVersion::PC || c->version == GameVersion::PATCH ||
      c->version == GameVersion::BB) {
    send_menu_t<S_MenuEntry_PC_BB_07>(c, menu_name, menu_id, items, is_info_menu);
  } else {
    send_menu_t<S_MenuEntry_DC_GC_07>(c, menu_name, menu_id, items, is_info_menu);
  }
}

////////////////////////////////////////////////////////////////////////////////
// CommandGameSelect: presents the player with a Game Select menu. returns the selection in the same way as CommandShipSelect.

template <typename CharT>
void send_game_menu_t(shared_ptr<Client> c, shared_ptr<ServerState> s) {
  vector<S_GameMenuEntry<CharT>> entries;
  {
    auto& e = entries.emplace_back();
    e.menu_id = GAME_MENU_ID;
    e.game_id = 0x00000000;
    e.difficulty_tag = 0x00;
    e.num_players = 0x00;
    e.name = s->name;
    e.episode = 0x00;
    e.flags = 0x04;
  }
  for (shared_ptr<Lobby> l : s->all_lobbies()) {
    if (!l->is_game() || (l->version != c->version)) {
      continue;
    }
    bool l_is_ep3 = !!(l->flags & LobbyFlag::EPISODE_3);
    bool c_is_ep3 = !!(c->flags & ClientFlag::EPISODE_3_GAMES);
    if (l_is_ep3 != c_is_ep3) {
      continue;
    }

    auto& e = entries.emplace_back();
    e.menu_id = GAME_MENU_ID;
    e.game_id = l->lobby_id;
    e.difficulty_tag = (l_is_ep3 ? 0x0A : (l->difficulty + 0x22));
    e.num_players = l->count_clients();
    e.episode = ((c->version == GameVersion::BB) ? (l->max_clients << 4) : 0) | l->episode;
    if (l->flags & LobbyFlag::EPISODE_3) {
      e.flags = (l->password.empty() ? 0 : 2);
    } else {
      e.flags = ((l->episode << 6) | ((l->mode % 3) << 4) | (l->password.empty() ? 0 : 2)) | ((l->mode == 3) ? 4 : 0);
    }
    e.name = l->name;
  }

  send_command(c, 0x08, entries.size() - 1, entries);
}

void send_game_menu(shared_ptr<Client> c, shared_ptr<ServerState> s) {
  if ((c->version == GameVersion::DC) || (c->version == GameVersion::GC)) {
    send_game_menu_t<char>(c, s);
  } else {
    send_game_menu_t<char16_t>(c, s);
  }
}



template <typename EntryT>
void send_quest_menu_t(
    shared_ptr<Client> c,
    uint32_t menu_id,
    const vector<shared_ptr<const Quest>>& quests,
    bool is_download_menu) {
  vector<EntryT> entries;
  for (const auto& quest : quests) {
    auto& e = entries.emplace_back();
    e.menu_id = menu_id;
    e.item_id = quest->quest_id;
    e.name = quest->name;
    e.short_desc = quest->short_description;
    add_color_inplace(e.short_desc);
  }
  send_command(c, is_download_menu ? 0xA4 : 0xA2, entries.size(), entries);
}

template <typename EntryT>
void send_quest_menu_t(
    shared_ptr<Client> c,
    uint32_t menu_id,
    const vector<MenuItem>& items,
    bool is_download_menu) {
  vector<EntryT> entries;
  for (const auto& item : items) {
    auto& e = entries.emplace_back();
    e.menu_id = menu_id;
    e.item_id = item.item_id;
    e.name = item.name;
    e.short_desc = item.description;
    add_color_inplace(e.short_desc);
  }
  send_command(c, is_download_menu ? 0xA4 : 0xA2, entries.size(), entries);
}

void send_quest_menu(shared_ptr<Client> c, uint32_t menu_id,
    const vector<shared_ptr<const Quest>>& quests, bool is_download_menu) {
  if (c->version == GameVersion::PC) {
    send_quest_menu_t<S_QuestMenuEntry_PC_A2_A4>(c, menu_id, quests, is_download_menu);
  } else if (c->version == GameVersion::GC) {
    send_quest_menu_t<S_QuestMenuEntry_GC_A2_A4>(c, menu_id, quests, is_download_menu);
  } else if (c->version == GameVersion::BB) {
    send_quest_menu_t<S_QuestMenuEntry_BB_A2_A4>(c, menu_id, quests, is_download_menu);
  } else {
    throw logic_error("unimplemented versioned command");
  }
}

void send_quest_menu(shared_ptr<Client> c, uint32_t menu_id,
    const std::vector<MenuItem>& items, bool is_download_menu) {
  if (c->version == GameVersion::PC) {
    send_quest_menu_t<S_QuestMenuEntry_PC_A2_A4>(c, menu_id, items, is_download_menu);
  } else if (c->version == GameVersion::GC) {
    send_quest_menu_t<S_QuestMenuEntry_GC_A2_A4>(c, menu_id, items, is_download_menu);
  } else if (c->version == GameVersion::BB) {
    send_quest_menu_t<S_QuestMenuEntry_BB_A2_A4>(c, menu_id, items, is_download_menu);
  } else {
    throw logic_error("unimplemented versioned command");
  }
}

void send_lobby_list(shared_ptr<Client> c, shared_ptr<ServerState> s) {
  // This command appears to be deprecated, as PSO expects it to be exactly how
  // this server sends it, and does not react if it's different, except by
  // changing the lobby IDs.

  vector<S_LobbyListEntry_83> entries;
  for (shared_ptr<Lobby> l : s->all_lobbies()) {
    if (!(l->flags & LobbyFlag::DEFAULT)) {
      continue;
    }
    if ((l->flags & LobbyFlag::EPISODE_3) && !(c->flags & ClientFlag::EPISODE_3_GAMES)) {
      continue;
    }
    auto& e = entries.emplace_back();
    e.menu_id = LOBBY_MENU_ID;
    e.item_id = l->lobby_id;
    e.unused = 0;
  }

  send_command(c, 0x83, entries.size(), entries);
}



////////////////////////////////////////////////////////////////////////////////
// lobby joining

template <typename LobbyDataT, typename DispDataT>
void send_join_game_t(shared_ptr<Client> c, shared_ptr<Lobby> l) {
  S_JoinGame<LobbyDataT, DispDataT> cmd;

  cmd.variations = l->variations;

  size_t player_count = 0;
  for (size_t x = 0; x < 4; x++) {
    if (l->clients[x]) {
      cmd.lobby_data[x].player_tag = 0x00010000;
      cmd.lobby_data[x].guild_card = l->clients[x]->license->serial_number;
      // See comment in send_join_lobby_t about Episode III behavior here
      cmd.lobby_data[x].ip_address = 0x7F000001;
      cmd.lobby_data[x].client_id = c->lobby_client_id;
      cmd.lobby_data[x].name = l->clients[x]->player.disp.name;
      if (l->flags & LobbyFlag::EPISODE_3) {
        cmd.players_ep3[x].inventory = l->clients[x]->player.inventory;
        cmd.players_ep3[x].disp = convert_player_disp_data<DispDataT>(
            l->clients[x]->player.disp);
      }
      player_count++;
    }
  }

  cmd.client_id = c->lobby_client_id;
  cmd.leader_id = l->leader_id;
  cmd.disable_udp = 0x01; // TODO: This is unused on PC/BB. Is it OK to use 1 here anyway?
  cmd.difficulty = l->difficulty;
  cmd.battle_mode = (l->mode == 1) ? 1 : 0;
  cmd.event = l->event;
  cmd.section_id = l->section_id;
  cmd.challenge_mode = (l->mode == 2) ? 1 : 0;
  cmd.rare_seed = l->rare_seed;
  cmd.episode = l->episode;
  cmd.unused2 = 0x01;
  cmd.solo_mode = (l->mode == 3);
  cmd.unused3 = 0x00;

  // Player data is only sent in Episode III games; in other versions, the
  // players send each other their data using 62/6D commands during loading
  size_t data_size = (l->flags & LobbyFlag::EPISODE_3)
      ? sizeof(cmd) : (sizeof(cmd) - sizeof(cmd.players_ep3));
  send_command(c, 0x64, player_count, &cmd, data_size);
}

template <typename LobbyDataT, typename DispDataT>
void send_join_lobby_t(shared_ptr<Client> c, shared_ptr<Lobby> l,
    shared_ptr<Client> joining_client = nullptr) {
  uint8_t command;
  if (l->is_game()) {
    if (joining_client) {
      command = 0x65;
    } else {
      throw logic_error("send_join_lobby_t should not be used for primary game join command");
    }
  } else {
    command = joining_client ? 0x68 : 0x67;
  }

  uint8_t lobby_type = (l->type > 14) ? (l->block - 1) : l->type;
  // Allow non-canonical lobby types on GC
  if (c->version == GameVersion::GC) {
    if (c->flags & ClientFlag::EPISODE_3_GAMES) {
      if ((l->type > 0x14) && (l->type < 0xE9)) {
        lobby_type = l->block - 1;
      }
    } else {
      if ((l->type > 0x11) && (l->type != 0x67) && (l->type != 0xD4) && (l->type < 0xFC)) {
        lobby_type = l->block - 1;
      }
    }
  } else {
    if (lobby_type > 0x0E) {
      lobby_type = l->block - 1;
    }
  }

  S_JoinLobby<LobbyDataT, DispDataT> cmd;
  cmd.client_id = c->lobby_client_id;
  cmd.leader_id = l->leader_id;
  cmd.disable_udp = 0x01;
  cmd.lobby_number = lobby_type;
  cmd.block_number = l->block;
  cmd.event = l->event;
  cmd.unused = 0x00000000;

  vector<shared_ptr<Client>> lobby_clients;
  if (joining_client) {
    lobby_clients.emplace_back(joining_client);
  } else {
    for (auto lc : l->clients) {
      if (lc) {
        lobby_clients.emplace_back(lc);
      }
    }
  }

  size_t used_entries = 0;
  for (const auto& lc : lobby_clients) {
    auto& e = cmd.entries[used_entries++];
    e.lobby_data.player_tag = 0x00010000;
    e.lobby_data.guild_card = lc->license->serial_number;
    // There's a strange behavior (bug? "feature"?) in Episode 3 where the start
    // button does nothing in the lobby (hence you can't "quit game") if the
    // client's IP address is zero. So, we fill it in with a fake nonzero value
    // to avoid this behavior.
    e.lobby_data.ip_address = 0x7F000001;
    e.lobby_data.client_id = lc->lobby_client_id;
    e.lobby_data.name = lc->player.disp.name;
    e.inventory = lc->player.inventory;
    e.disp = convert_player_disp_data<DispDataT>(lc->player.disp);
    if (c->version == GameVersion::PC) {
      e.disp.enforce_pc_limits();
    }
  }

  send_command(c, command, used_entries, &cmd, cmd.size(used_entries));
}

void send_join_lobby(shared_ptr<Client> c, shared_ptr<Lobby> l) {
  if (l->is_game()) {
    if (c->version == GameVersion::PC) {
      send_join_game_t<PlayerLobbyDataPC, PlayerDispDataPCGC>(c, l);
    } else if (c->version == GameVersion::GC) {
      send_join_game_t<PlayerLobbyDataGC, PlayerDispDataPCGC>(c, l);
    } else if (c->version == GameVersion::BB) {
      send_join_game_t<PlayerLobbyDataBB, PlayerDispDataBB>(c, l);
    } else {
      throw logic_error("unimplemented versioned command");
    }
  } else {
    if (c->version == GameVersion::PC) {
      send_join_lobby_t<PlayerLobbyDataPC, PlayerDispDataPCGC>(c, l);
    } else if (c->version == GameVersion::GC) {
      send_join_lobby_t<PlayerLobbyDataGC, PlayerDispDataPCGC>(c, l);
    } else if (c->version == GameVersion::BB) {
      send_join_lobby_t<PlayerLobbyDataBB, PlayerDispDataBB>(c, l);
    } else {
      throw logic_error("unimplemented versioned command");
    }
  }

  // If the client will stop sending message box close confirmations after
  // joining any lobby, set the appropriate flag and update the client config
  if ((c->flags & (ClientFlag::NO_MESSAGE_BOX_CLOSE_CONFIRMATION_AFTER_LOBBY_JOIN | ClientFlag::NO_MESSAGE_BOX_CLOSE_CONFIRMATION))
      == ClientFlag::NO_MESSAGE_BOX_CLOSE_CONFIRMATION_AFTER_LOBBY_JOIN) {
    c->flags |= ClientFlag::NO_MESSAGE_BOX_CLOSE_CONFIRMATION;
    send_update_client_config(c);
  }
}

void send_player_join_notification(shared_ptr<Client> c,
    shared_ptr<Lobby> l, shared_ptr<Client> joining_client) {
  if (c->version == GameVersion::PC) {
    send_join_lobby_t<PlayerLobbyDataPC, PlayerDispDataPCGC>(c, l, joining_client);
  } else if (c->version == GameVersion::GC) {
    send_join_lobby_t<PlayerLobbyDataGC, PlayerDispDataPCGC>(c, l, joining_client);
  } else if (c->version == GameVersion::BB) {
    send_join_lobby_t<PlayerLobbyDataBB, PlayerDispDataBB>(c, l, joining_client);
  } else {
    throw logic_error("unimplemented versioned command");
  }
}

void send_player_leave_notification(shared_ptr<Lobby> l, uint8_t leaving_client_id) {
  S_LeaveLobby_66_69 cmd = {leaving_client_id, l->leader_id, 0};
  send_command(l, l->is_game() ? 0x66 : 0x69, leaving_client_id, cmd);
}

void send_get_player_info(shared_ptr<Client> c) {
  send_command(c, 0x95);
}



////////////////////////////////////////////////////////////////////////////////
// arrows

void send_arrow_update(shared_ptr<Lobby> l) {
  vector<S_ArrowUpdateEntry_88> entries;

  for (size_t x = 0; x < l->max_clients; x++) {
    if (!l->clients[x]) {
      continue;
    }
    auto& e = entries.emplace_back();
    e.player_tag = 0x00010000;
    e.serial_number = l->clients[x]->license->serial_number;
    e.arrow_color = l->clients[x]->lobby_arrow_color;
  }

  send_command(l, 0x88, entries.size(), entries);
}

// tells the player that the joining player is done joining, and the game can resume
void send_resume_game(shared_ptr<Lobby> l, shared_ptr<Client> ready_client) {
  uint32_t data = 0x081C0372;
  send_command_excluding_client(l, ready_client, 0x60, 0x00, &data, 4);
}



////////////////////////////////////////////////////////////////////////////////
// Game/cheat commands

// sends an HP/TP/Meseta modifying command (see flag definitions in command-functions.h)
void send_player_stats_change(shared_ptr<Lobby> l, shared_ptr<Client> c,
    PlayerStatsChange stat, uint32_t amount) {

  if (amount > 2550) {
    throw invalid_argument("amount cannot be larger than 2550");
  }

  vector<PSOSubcommand> subs;
  while (amount > 0) {
    {
      auto& sub = subs.emplace_back();
      sub.byte[0] = 0x9A;
      sub.byte[1] = 0x02;
      sub.byte[2] = c->lobby_client_id;
      sub.byte[3] = 0x00;
    }
    {
      auto& sub = subs.emplace_back();
      sub.byte[0] = 0x00;
      sub.byte[1] = 0x00;
      sub.byte[2] = stat;
      sub.byte[3] = (amount > 0xFF) ? 0xFF : amount;
      amount -= sub.byte[3];
    }
  }

  send_command(l, 0x60, 0x00, subs);
}

void send_warp(shared_ptr<Client> c, uint32_t area) {
  PSOSubcommand cmds[2];
  cmds[0].byte[0] = 0x94;
  cmds[0].byte[1] = 0x02;
  cmds[0].byte[2] = c->lobby_client_id;
  cmds[0].byte[3] = 0x00;
  cmds[1].dword = area;
  send_command(c, 0x62, c->lobby_client_id, cmds, 8);
}

void send_ep3_change_music(shared_ptr<Client> c, uint32_t song) {
  PSOSubcommand cmds[2];
  cmds[0].byte[0] = 0xBF;
  cmds[0].byte[1] = 0x02;
  cmds[0].byte[2] = c->lobby_client_id;
  cmds[0].byte[3] = 0x00;
  cmds[1].dword = song;
  send_command(c, 0x60, 0x00, cmds, 8);
}

void send_set_player_visibility(shared_ptr<Lobby> l, shared_ptr<Client> c,
    bool visible) {
  PSOSubcommand cmd;
  cmd.byte[0] = visible ? 0x23 : 0x22;
  cmd.byte[1] = 0x01;
  cmd.byte[2] = c->lobby_client_id;
  cmd.byte[3] = 0x00;
  send_command(l, 0x60, 0x00, &cmd, 4);
}

void send_revive_player(shared_ptr<Lobby> l, shared_ptr<Client> c) {
  PSOSubcommand cmd;
  cmd.byte[0] = 0x31;
  cmd.byte[1] = 0x01;
  cmd.byte[2] = c->lobby_client_id;
  cmd.byte[3] = 0x00;
  send_command(l, 0x60, 0x00, &cmd, 4);
}



////////////////////////////////////////////////////////////////////////////////
// BB game commands

// notifies other players of a dropped item from a box or enemy
void send_drop_item(shared_ptr<Lobby> l, const ItemData& item,
    bool from_enemy, uint8_t area, float x, float y, uint16_t request_id) {
  S_DropItem_BB cmd = {
      0x5F, 0x0A, 0x0000, area, from_enemy, request_id, x, y, 0, item};
  send_command(l, 0x60, 0x00, cmd);
}

// notifies other players that a stack was split and part of it dropped (a new item was created)
void send_drop_stacked_item(shared_ptr<Lobby> l, const ItemData& item,
    uint8_t area, float x, float y) {
  S_DropStackedItem_BB cmd = {
      0x5D, 0x09, 0x0000, area, 0, x, y, 0, item};
  send_command(l, 0x60, 0x00, cmd);
}

// notifies other players that an item was picked up
void send_pick_up_item(shared_ptr<Lobby> l, shared_ptr<Client> c,
    uint32_t item_id, uint8_t area) {
  S_PickUpItem_BB cmd = {
      0x59, 0x03, c->lobby_client_id, c->lobby_client_id, area, item_id};
  send_command(l, 0x60, 0x00, cmd);
}

// creates an item in a player's inventory (used for withdrawing items from the bank)
void send_create_inventory_item(shared_ptr<Lobby> l, shared_ptr<Client> c,
    const ItemData& item) {
  S_CreateInventoryItem_BB cmd = {
      0xBE, 0x07, c->lobby_client_id, item, 0};
  send_command(l, 0x60, 0x00, cmd);
}

// destroys an item
void send_destroy_item(shared_ptr<Lobby> l, shared_ptr<Client> c,
    uint32_t item_id, uint32_t amount) {
  S_DestroyItem_BB cmd = {
      0x29, 0x03, c->lobby_client_id, item_id, amount};
  send_command(l, 0x60, 0x00, cmd);
}

// sends the player their bank data
void send_bank(shared_ptr<Client> c) {
  vector<PlayerBankItem> items(c->player.bank.items,
      &c->player.bank.items[c->player.bank.num_items]);

  uint32_t checksum = random_object<uint32_t>();
  S_BankContentsHeader_BB cmd = {
      0xBC, 0, 0, 0, checksum, c->player.bank.num_items, c->player.bank.meseta};

  size_t size = 8 + sizeof(cmd) + items.size() * sizeof(PlayerBankItem);
  cmd.size = size;

  send_command(c, 0x6C, 0x00, cmd, items);
}

// sends the player a shop's contents
void send_shop(shared_ptr<Client> c, uint8_t shop_type) {
  S_ShopContents_BB cmd = {
    0xB6,
    0x2C,
    0x037F,
    shop_type,
    static_cast<uint8_t>(c->player.current_shop_contents.size()),
    0,
    {},
  };

  size_t count = c->player.current_shop_contents.size();
  if (count > sizeof(cmd.entries) / sizeof(cmd.entries[0])) {
    throw logic_error("too many items in shop");
  }

  for (size_t x = 0; x < count; x++) {
    cmd.entries[x] = c->player.current_shop_contents[x];
  }

  send_command(c, 0x6C, 0x00, &cmd, sizeof(cmd) - sizeof(cmd.entries[0]) * (20 - count));
}

// notifies players about a level up
void send_level_up(shared_ptr<Lobby> l, shared_ptr<Client> c) {
  PlayerStats stats = c->player.disp.stats;

  for (size_t x = 0; x < c->player.inventory.num_items; x++) {
    if ((c->player.inventory.items[x].equip_flags & 0x08) &&
        (c->player.inventory.items[x].data.item_data1[0] == 0x02)) {
      stats.dfp += (c->player.inventory.items[x].data.item_data1w[2] / 100);
      stats.atp += (c->player.inventory.items[x].data.item_data1w[3] / 50);
      stats.ata += (c->player.inventory.items[x].data.item_data1w[4] / 200);
      stats.mst += (c->player.inventory.items[x].data.item_data1w[5] / 50);
    }
  }

  // TODO: Make a real struct for this
  PSOSubcommand sub[5];
  sub[0].byte[0] = 0x30;
  sub[0].byte[1] = 0x05;
  sub[0].word[1] = c->lobby_client_id;
  sub[1].word[0] = stats.atp;
  sub[1].word[1] = stats.mst;
  sub[2].word[0] = stats.evp;
  sub[2].word[1] = stats.hp;
  sub[3].word[0] = stats.dfp;
  sub[3].word[1] = stats.ata;
  sub[4].dword = c->player.disp.level;
  send_command(l, 0x60, 0x00, sub, 0x14);
}

// gives a player EXP
void send_give_experience(shared_ptr<Lobby> l, shared_ptr<Client> c,
    uint32_t amount) {
  // TODO: Make a real struct for this
  PSOSubcommand sub[2];
  sub[0].word[0] = 0x02BF;
  sub[0].word[1] = c->lobby_client_id;
  sub[1].dword = amount;
  send_command(l, 0x60, 0x00, sub, 8);
}



////////////////////////////////////////////////////////////////////////////////
// ep3 only commands

// sends the (PRS-compressed) card list to the client
void send_ep3_card_list_update(shared_ptr<Client> c) {
  auto file_data = file_cache.get("system/ep3/cardupdate.mnr");

  StringWriter w;
  w.put_u32l(file_data->size());
  w.write(*file_data);

  send_command(c, 0xB8, 0x00, w.str());
}

// sends the client a generic rank
void send_ep3_rank_update(shared_ptr<Client> c) {
  S_RankUpdate_GC_Ep3_B7 cmd = {
      0, "\0\0\0\0\0\0\0\0\0\0\0", 0x00FFFFFF, 0x00FFFFFF, 0xFFFFFFFF};
  send_command(c, 0xB7, 0x00, cmd);
}

// sends the map list (used for battle setup) to all players in a game
void send_ep3_map_list(shared_ptr<Lobby> l) {
  auto file_data = file_cache.get("system/ep3/maplist.mnr");

  string data(16, '\0');
  PSOSubcommand* subs = reinterpret_cast<PSOSubcommand*>(data.data());
  subs[0].dword = 0x000000B6;
  subs[1].dword = (23 + file_data->size()) & 0xFFFFFFFC;
  subs[2].dword = 0x00000040;
  subs[3].dword = file_data->size();
  data += *file_data;

  send_command(l, 0x6C, 0x00, data);
}

// sends the map data for the chosen map to all players in the game
void send_ep3_map_data(shared_ptr<Lobby> l, uint32_t map_id) {
  string filename = string_printf("system/ep3/map%08" PRIX32 ".mnm", map_id);
  auto file_data = file_cache.get(filename);

  string data(12, '\0');
  PSOSubcommand* subs = reinterpret_cast<PSOSubcommand*>(data.data());
  subs[0].dword = 0x000000B6;
  subs[1].dword = (19 + file_data->size()) & 0xFFFFFFFC;
  subs[2].dword = 0x00000041;
  data += *file_data;

  send_command(l, 0x6C, 0x00, data);
}



template <typename CommandT>
void send_quest_open_file_t(
    shared_ptr<Client> c,
    const string& filename,
    uint32_t file_size,
    bool is_download_quest,
    bool is_ep3_quest) {
  CommandT cmd;
  cmd.flags = 2 + is_ep3_quest;
  cmd.file_size = file_size;
  cmd.name = filename.c_str();
  cmd.filename = filename.c_str();
  send_command(c, is_download_quest ? 0xA6 : 0x44, 0x00, cmd);
}

void send_quest_file_chunk(
    shared_ptr<Client> c,
    const char* filename,
    size_t chunk_index,
    const void* data,
    size_t size,
    bool is_download_quest) {
  if (size > 0x400) {
    throw invalid_argument("quest file chunks must be 1KB or smaller");
  }

  S_WriteFile_13_A7 cmd;
  cmd.filename = filename;
  memcpy(cmd.data, data, size);
  if (size < 0x400) {
    memset(&cmd.data[size], 0, 0x400 - size);
  }
  cmd.data_size = size;

  send_command(c, is_download_quest ? 0xA7 : 0x13, chunk_index, cmd);
}

void send_quest_file(shared_ptr<Client> c, const string& basename,
    const string& contents, bool is_download_quest, bool is_ep3_quest) {

  if (c->version == GameVersion::PC || c->version == GameVersion::GC) {
    send_quest_open_file_t<S_OpenFile_PC_GC_44_A6>(
        c, basename, contents.size(), is_download_quest, is_ep3_quest);
  } else if (c->version == GameVersion::BB) {
    send_quest_open_file_t<S_OpenFile_BB_44_A6>(
        c, basename, contents.size(), is_download_quest, is_ep3_quest);
  } else {
    throw invalid_argument("cannot send quest files to this version of client");
  }

  for (size_t offset = 0; offset < contents.size(); offset += 0x400) {
    size_t chunk_bytes = contents.size() - offset;
    if (chunk_bytes > 0x400) {
      chunk_bytes = 0x400;
    }
    send_quest_file_chunk(c, basename.c_str(), offset / 0x400,
        contents.data() + offset, chunk_bytes, is_download_quest);
  }
}

void send_server_time(shared_ptr<Client> c) {
  uint64_t t = now();

  time_t t_secs = t / 1000000;
  struct tm t_parsed;
  gmtime_r(&t_secs, &t_parsed);

  string time_str(128, 0);
  size_t len = strftime(time_str.data(), time_str.size(),
      "%Y:%m:%d: %H:%M:%S.000", &t_parsed);
  if (len == 0) {
    throw runtime_error("format_time buffer too short");
  }
  time_str.resize(len);

  send_command(c, 0xB1, 0x00, time_str);
}

void send_change_event(shared_ptr<Client> c, uint8_t new_event) {
  send_command(c, 0xDA, new_event);
}

void send_change_event(shared_ptr<Lobby> l, uint8_t new_event) {
  send_command(l, 0xDA, new_event);
}

void send_change_event(shared_ptr<ServerState> s, uint8_t new_event) {
  send_command(s, 0xDA, new_event);
}
