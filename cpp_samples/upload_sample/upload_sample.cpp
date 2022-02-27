﻿#include <iostream>

#include "logging.h"
#include "converting.h"
#include "messaging_client.h"
#include "folder_handler.h"
#include "argument_parser.h"

#ifndef __USE_TYPE_CONTAINER__
#include "cpprest/json.h"
#else
#include "container.h"
#include "values/string_value.h"
#include "values/container_value.h"
#endif

#include "fmt/xchar.h"
#include "fmt/format.h"

#include <future>

constexpr auto PROGRAM_NAME = L"upload_sample";

using namespace std;
using namespace logging;
using namespace network;
using namespace converting;
using namespace folder_handler;
using namespace argument_parser;

bool write_console = false;
bool encrypt_mode = false;
bool compress_mode = false;
logging_level log_level = logging_level::information;
wstring source_folder = L"";
wstring target_folder = L"";
wstring connection_key = L"middle_connection_key";
wstring server_ip = L"127.0.0.1";
unsigned short server_port = 8642;
unsigned short high_priority_count = 1;
unsigned short normal_priority_count = 2;
unsigned short low_priority_count = 3;

promise<bool> _promise_status;
future<bool> _future_status;

#ifndef __USE_TYPE_CONTAINER__
map<wstring, function<void(shared_ptr<json::value>)>> _registered_messages;
#else
map<wstring, function<void(shared_ptr<container::value_container>)>> _registered_messages;
#endif

bool parse_arguments(const map<wstring, wstring>& arguments);
void connection(const wstring& target_id, const wstring& target_sub_id, const bool& condition);

#ifndef __USE_TYPE_CONTAINER__
void received_message(shared_ptr<json::value> container);
void transfer_condition(shared_ptr<json::value> container);
#else
void received_message(shared_ptr<container::value_container> container);
void transfer_condition(shared_ptr<container::value_container> container);
#endif

void display_help(void);

int main(int argc, char* argv[])
{
	if (!parse_arguments(argument::parse(argc, argv)))
	{
		return 0;
	}

	vector<wstring> sources = folder::get_files(source_folder);
	if (sources.empty())
	{
		display_help();

		return 0;
	}

	logger::handle().set_write_console(write_console);
	logger::handle().set_target_level(log_level);
	logger::handle().start(PROGRAM_NAME);

	_registered_messages.insert({ L"transfer_condition", transfer_condition });

	shared_ptr<messaging_client> client = make_shared<messaging_client>(PROGRAM_NAME);
	client->set_compress_mode(compress_mode);
	client->set_connection_key(connection_key);
	client->set_session_types(session_types::message_line);
	client->set_connection_notification(&connection);
	client->set_message_notification(&received_message);
	client->start(server_ip, server_port, high_priority_count, normal_priority_count, low_priority_count);

#ifndef __USE_TYPE_CONTAINER__
	shared_ptr<json::value> container = make_shared<json::value>(json::value::object(true));

	(*container)[L"header"][L"source_id"] = json::value::string(L"");
	(*container)[L"header"][L"source_sub_id"] = json::value::string(L"");
	(*container)[L"header"][L"target_id"] = json::value::string(L"main_server");
	(*container)[L"header"][L"target_sub_id"] = json::value::string(L"");
	(*container)[L"header"][L"message_type"] = json::value::string(L"download_files");

	(*container)[L"data"][L"indication_id"] = json::value::string(L"download_test");

	int index = 0;
	(*container)[L"data"][L"files"] = json::value::array();
	for (auto& source : sources)
	{
		(*container)[L"data"][L"files"][index][L"source"] = json::value::string(source);
		(*container)[L"data"][L"files"][index][L"target"] = json::value::string(converter::replace2(source, source_folder, target_folder));
		index++;
	}
#else
	vector<shared_ptr<container::value>> files;

	files.push_back(make_shared<container::string_value>(L"indication_id", L"upload_test"));
	for (auto& source : sources)
	{
		files.push_back(make_shared<container::container_value>(L"file", vector<shared_ptr<container::value>> {
			make_shared<container::string_value>(L"source", source),
			make_shared<container::string_value>(L"target", converter::replace2(source, source_folder, target_folder))
		}));
	}

	shared_ptr<container::value_container> container =
		make_shared<container::value_container>(L"main_server", L"", L"upload_files", files);
#endif

	_future_status = _promise_status.get_future();

	client->send(container);

	_future_status.wait_for(chrono::seconds(100));

	client->stop();

	logger::handle().stop();

	return 0;
}

bool parse_arguments(const map<wstring, wstring>& arguments)
{
	wstring temp;

	auto target = arguments.find(L"--help");
	if (target != arguments.end())
	{
		display_help();

		return false;
	}

	target = arguments.find(L"--encrypt_mode");
	if (target != arguments.end())
	{
		temp = target->second;
		transform(temp.begin(), temp.end(), temp.begin(), ::tolower);

		if (temp.compare(L"true") == 0)
		{
			encrypt_mode = true;
		}
		else
		{
			encrypt_mode = false;
		}
	}

	target = arguments.find(L"--compress_mode");
	if (target != arguments.end())
	{
		temp = target->second;
		transform(temp.begin(), temp.end(), temp.begin(), ::tolower);

		if (temp.compare(L"true") == 0)
		{
			compress_mode = true;
		}
		else
		{
			compress_mode = false;
		}
	}

	target = arguments.find(L"--connection_key");
	if (target != arguments.end())
	{
		connection_key = target->second;
	}

	target = arguments.find(L"--server_ip");
	if (target != arguments.end())
	{
		server_ip = target->second;
	}

	target = arguments.find(L"--server_port");
	if (target != arguments.end())
	{
		server_port = (unsigned short)_wtoi(target->second.c_str());
	}

	target = arguments.find(L"--source_folder");
	if (target != arguments.end())
	{
		source_folder = target->second;
	}

	target = arguments.find(L"--target_folder");
	if (target != arguments.end())
	{
		target_folder = target->second;
	}

	target = arguments.find(L"--high_priority_count");
	if (target != arguments.end())
	{
		high_priority_count = (unsigned short)_wtoi(target->second.c_str());
	}

	target = arguments.find(L"--normal_priority_count");
	if (target != arguments.end())
	{
		normal_priority_count = (unsigned short)_wtoi(target->second.c_str());
	}

	target = arguments.find(L"--low_priority_count");
	if (target != arguments.end())
	{
		low_priority_count = (unsigned short)_wtoi(target->second.c_str());
	}

	target = arguments.find(L"--write_console_mode");
	if (target != arguments.end())
	{
		temp = target->second;
		transform(temp.begin(), temp.end(), temp.begin(), ::tolower);

		if (temp.compare(L"true") == 0)
		{
			write_console = true;
		}
		else
		{
			write_console = false;
		}
	}

	target = arguments.find(L"--logging_level");
	if (target != arguments.end())
	{
		log_level = (logging_level)_wtoi(target->second.c_str());
	}

	return true;
}

void connection(const wstring& target_id, const wstring& target_sub_id, const bool& condition)
{
	logger::handle().write(logging_level::information,
		fmt::format(L"a client on main server: {}[{}] is {}", target_id, target_sub_id, condition ? L"connected" : L"disconnected"));
}

#ifndef __USE_TYPE_CONTAINER__
void received_message(shared_ptr<json::value> container)
#else
void received_message(shared_ptr<container::value_container> container)
#endif
{
	if (container == nullptr)
	{
		return;
	}

#ifndef __USE_TYPE_CONTAINER__
	auto message_type = _registered_messages.find((*container)[L"header"][L"message_type"].as_string());
#else
	auto message_type = _registered_messages.find(container->message_type());
#endif
	if (message_type != _registered_messages.end())
	{
		message_type->second(container);

		return;
	}

	logger::handle().write(logging_level::sequence, fmt::format(L"unknown message: {}", container->serialize()));
}

#ifndef __USE_TYPE_CONTAINER__
void transfer_condition(shared_ptr<json::value> container)
#else
void transfer_condition(shared_ptr<container::value_container> container)
#endif
{
	if (container == nullptr)
	{
		return;
	}

#ifndef __USE_TYPE_CONTAINER__
	if ((*container)[L"header"][L"message_type"].as_string() != L"transfer_condition")
#else
	if (container->message_type() != L"transfer_condition")
#endif
	{
		return;
	}

#ifndef __USE_TYPE_CONTAINER__
	if ((*container)[L"data"][L"percentage"].as_integer() == 0)
#else
	if (container->get_value(L"percentage")->to_ushort() == 0)
#endif
	{
#ifndef __USE_TYPE_CONTAINER__
		logger::handle().write(logging_level::information,
			fmt::format(L"started upload: [{}]", (*container)[L"data"][L"indication_id"].as_string()));
#else
		logger::handle().write(logging_level::information,
			fmt::format(L"started upload: [{}]", container->get_value(L"indication_id")->to_string()));
#endif

		return;
	}

#ifndef __USE_TYPE_CONTAINER__
	logger::handle().write(logging_level::information,
		fmt::format(L"received percentage: [{}] {}%", (*container)[L"data"][L"indication_id"].as_string(),
			(*container)[L"data"][L"percentage"].as_integer()));
#else
	logger::handle().write(logging_level::information,
		fmt::format(L"received percentage: [{}] {}%", container->get_value(L"indication_id")->to_string(),
			container->get_value(L"percentage")->to_ushort()));
#endif

#ifndef __USE_TYPE_CONTAINER__
	if ((*container)[L"data"][L"completed"].as_bool())
#else
	if (container->get_value(L"completed")->to_boolean())
#endif
	{
#ifndef __USE_TYPE_CONTAINER__
		logger::handle().write(logging_level::information,
			fmt::format(L"completed upload: [{}] success-{}, fail-{}", (*container)[L"data"][L"indication_id"].as_string(),
				(*container)[L"data"][L"completed_count"].as_integer(), (*container)[L"data"][L"failed_count"].as_integer()));
#else
		logger::handle().write(logging_level::information,
			fmt::format(L"completed upload: [{}] success-{}, fail-{}", container->get_value(L"indication_id")->to_string(),
				container->get_value(L"completed_count")->to_ushort(), container->get_value(L"failed_count")->to_ushort()));
#endif

		_promise_status.set_value(false);
	}
#ifndef __USE_TYPE_CONTAINER__
	if ((*container)[L"data"][L"percentage"].as_integer() == 100)
#else
	else if (container->get_value(L"percentage")->to_ushort() == 100)
#endif
	{
#ifndef __USE_TYPE_CONTAINER__
		logger::handle().write(logging_level::information,
			fmt::format(L"completed upload: [{}]", (*container)[L"data"][L"indication_id"].as_string()));
#else
		logger::handle().write(logging_level::information,
			fmt::format(L"completed upload: [{}]", container->get_value(L"indication_id")->to_string()));
#endif

		_promise_status.set_value(true);
	}
}

void display_help(void)
{
	wcout << L"download sample options:" << endl << endl;
	wcout << L"--encrypt_mode [value] " << endl;
	wcout << L"\tThe encrypt_mode on/off. If you want to use encrypt mode must be appended '--encrypt_mode true'.\n\tInitialize value is --encrypt_mode off." << endl << endl;
	wcout << L"--compress_mode [value]" << endl;
	wcout << L"\tThe compress_mode on/off. If you want to use compress mode must be appended '--compress_mode true'.\n\tInitialize value is --compress_mode off." << endl << endl;
	wcout << L"--connection_key [value]" << endl;
	wcout << L"\tIf you want to change a specific key string for the connection to the main server must be appended\n\t'--connection_key [specific key string]'." << endl << endl;
	wcout << L"--server_port [value]" << endl;
	wcout << L"\tIf you want to change a port number for the connection to the main server must be appended\n\t'--server_port [port number]'." << endl << endl;
	wcout << L"--high_priority_count [value]" << endl;
	wcout << L"\tIf you want to change high priority thread workers must be appended '--high_priority_count [count]'." << endl << endl;
	wcout << L"--normal_priority_count [value]" << endl;
	wcout << L"\tIf you want to change normal priority thread workers must be appended '--normal_priority_count [count]'." << endl << endl;
	wcout << L"--low_priority_count [value]" << endl;
	wcout << L"\tIf you want to change low priority thread workers must be appended '--low_priority_count [count]'." << endl << endl;
	wcout << L"--source_folder [path]" << endl;
	wcout << L"\tIf you want to download folder on middle server on computer must be appended '--source_folder [path]'." << endl << endl;
	wcout << L"--target_folder [path]" << endl;
	wcout << L"\tIf you want to download on your computer must be appended '--target_folder [path]'." << endl << endl;
	wcout << L"--write_console_mode [value] " << endl;
	wcout << L"\tThe write_console_mode on/off. If you want to display log on console must be appended '--write_console_mode true'.\n\tInitialize value is --write_console_mode off." << endl << endl;
	wcout << L"--logging_level [value]" << endl;
	wcout << L"\tIf you want to change log level must be appended '--logging_level [level]'." << endl;
}