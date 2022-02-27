﻿#include "messaging_session.h"

#ifdef __USE_TYPE_CONTAINER__
#include "values/bool_value.h"
#include "values/string_value.h"
#include "values/container_value.h"
#endif

#include "logging.h"
#include "converting.h"
#include "encrypting.h"
#include "compressing.h"
#include "thread_pool.h"
#include "thread_worker.h"
#include "job_pool.h"
#include "job.h"

#include "data_lengths.h"
#include "file_handler.h"

#include "fmt/xchar.h"
#include "fmt/format.h"

namespace network
{
	using namespace logging;
	using namespace threads;

#ifdef __USE_TYPE_CONTAINER__
	using namespace container;
#endif

	using namespace converting;
	using namespace encrypting;
	using namespace compressing;
	using namespace file_handler;

	messaging_session::messaging_session(const wstring& source_id, const wstring& connection_key, asio::ip::tcp::socket& socket)
		: data_handling(246, 135), _confirm(session_conditions::waiting), _compress_mode(false), _encrypt_mode(false), _bridge_line(false), _received_message(nullptr),
		_key(L""), _iv(L""), _thread_pool(nullptr), _source_id(source_id), _source_sub_id(L""), _target_id(L""), _target_sub_id(L""), 
		_connection_key(connection_key), _received_file(nullptr), _received_data(nullptr), _connection(nullptr), _kill_code(false),
		_socket(make_shared<asio::ip::tcp::socket>(move(socket)))
	{
		_socket->set_option(asio::ip::tcp::no_delay(true));
		_socket->set_option(asio::socket_base::keep_alive(true));
		_socket->set_option(asio::socket_base::receive_buffer_size(buffer_size));

		_source_sub_id = fmt::format(L"{}:{}",
			converter::to_wstring(_socket->local_endpoint().address().to_string()), _socket->local_endpoint().port());
		_target_sub_id = fmt::format(L"{}:{}",
			converter::to_wstring(_socket->remote_endpoint().address().to_string()), _socket->remote_endpoint().port());

		_message_handlers.insert({ L"request_connection", bind(&messaging_session::connection_message, this, placeholders::_1) });
		_message_handlers.insert({ L"request_files", bind(&messaging_session::request_files, this, placeholders::_1) });
		_message_handlers.insert({ L"echo", bind(&messaging_session::echo_message, this, placeholders::_1) });
	}

	messaging_session::~messaging_session(void)
	{
		if (_socket != nullptr)
		{
			if (_socket->is_open())
			{
				_socket->close();
			}
			_socket.reset();
		}

		stop();
	}

	shared_ptr<messaging_session> messaging_session::get_ptr(void)
	{
		return shared_from_this();
	}

	void messaging_session::set_kill_code(const bool& kill_code)
	{
		_kill_code = kill_code;
	}

	void messaging_session::set_ignore_target_ids(const vector<wstring>& ignore_target_ids)
	{
		_ignore_target_ids = ignore_target_ids;
	}

	void messaging_session::set_ignore_snipping_targets(const vector<wstring>& ignore_snipping_targets)
	{
		_ignore_snipping_targets = ignore_snipping_targets;
	}

	void messaging_session::set_connection_notification(const function<void(shared_ptr<messaging_session>, const bool&)>& notification)
	{
		_connection = notification;
	}

#ifndef __USE_TYPE_CONTAINER__
	void messaging_session::set_message_notification(const function<void(shared_ptr<json::value>)>& notification)
#else
	void messaging_session::set_message_notification(const function<void(shared_ptr<container::value_container>)>& notification)
#endif
	{
		_received_message = notification;
	}

	void messaging_session::set_file_notification(const function<void(const wstring&, const wstring&, const wstring&, const wstring&)>& notification)
	{
		_received_file = notification;
	}

	void messaging_session::set_binary_notification(const function<void(const wstring&, const wstring&, const wstring&, const wstring&, const vector<unsigned char>&)>& notification)
	{
		_received_data = notification;
	}

	const session_conditions messaging_session::get_confirom_status(void)
	{
		return _confirm;
	}

	const session_types messaging_session::get_session_type(void)
	{
		return _session_type;
	}

	const wstring messaging_session::target_id(void)
	{
		return _target_id;
	}

	const wstring messaging_session::target_sub_id(void)
	{
		return _target_sub_id;
	}

	void messaging_session::start(const bool& encrypt_mode, const bool& compress_mode, const vector<session_types>& possible_session_types, 
		const unsigned short& high_priority, const unsigned short& normal_priority, const unsigned short& low_priority)
	{
		stop();

		_encrypt_mode = encrypt_mode;
		_compress_mode = compress_mode;
		_possible_session_types = possible_session_types;
		_thread_pool = make_shared<threads::thread_pool>();

		_thread_pool->append(make_shared<thread_worker>(priorities::top), true);
		for (unsigned short high = 0; high < high_priority; ++high)
		{
			_thread_pool->append(make_shared<thread_worker>(priorities::high, vector<priorities> { priorities::normal, priorities::low }), true);
		}
		for (unsigned short normal = 0; normal < normal_priority; ++normal)
		{
			_thread_pool->append(make_shared<thread_worker>(priorities::normal, vector<priorities> { priorities::high, priorities::low }), true);
		}
		for (unsigned short low = 0; low < low_priority; ++low)
		{
			_thread_pool->append(make_shared<thread_worker>(priorities::low, vector<priorities> { priorities::high, priorities::normal }), true);
		}

		_thread_pool->push(make_shared<job>(priorities::high, bind(&messaging_session::check_confirm_condition, this)));

		read_start_code(_socket);

		logger::handle().write(logging_level::information, fmt::format(L"started session: {}:{}", 
			converter::to_wstring(_socket->remote_endpoint().address().to_string()), _socket->remote_endpoint().port()));
	}

	void messaging_session::stop(void)
	{
		if (_thread_pool != nullptr)
		{
			_thread_pool->stop();
			_thread_pool.reset();
		}
	}

	void messaging_session::echo(void)
	{
#ifndef __USE_TYPE_CONTAINER__
		shared_ptr<json::value> container = make_shared<json::value>(json::value::object(true));
#else
		shared_ptr<container::value_container> container = make_shared<container::value_container>(_source_id, _source_sub_id, _target_id, _target_sub_id, L"echo",
			vector<shared_ptr<container::value>> {});
#endif

		send(container);
	}

#ifndef __USE_TYPE_CONTAINER__
	void messaging_session::send(shared_ptr<json::value> message)
#else
	void messaging_session::send(shared_ptr<container::value_container> message)
#endif
	{
		if (message == nullptr)
		{
			return;
		}


#ifndef __USE_TYPE_CONTAINER__
		if (!_bridge_line && (*message)[L"header"][L"target_id"].as_string() != _target_id && !contained_snipping_target((*message)[L"header"][L"target_id"].as_string()))
		{
			return;
		}
		
		if (!_bridge_line && !contained_snipping_target((*message)[L"header"][L"target_id"].as_string()) && 
			!(*message)[L"header"][L"target_sub_id"].is_null() && (*message)[L"header"][L"target_sub_id"].as_string() != _target_sub_id)
		{
			return;
		}

		auto serialize_array = converter::to_array(message->serialize());
#else
		if (!_bridge_line && message->target_id() != _target_id && !contained_snipping_target(message->target_id()))
		{
			return;
		}

		if (!_bridge_line && !contained_snipping_target(message->target_id()) && !message->target_sub_id().empty() && message->target_sub_id() != _target_sub_id)
		{
			return;
		}

		auto serialize_array = message->serialize_array();
#endif

		logger::handle().write(logging_level::packet, serialize_array);

		if (_compress_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::high, serialize_array, bind(&messaging_session::compress_packet, this, placeholders::_1)));

			return;
		}

		if (_encrypt_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::normal, serialize_array, bind(&messaging_session::encrypt_packet, this, placeholders::_1)));

			return;
		}

		_thread_pool->push(make_shared<job>(priorities::top, serialize_array, bind(&messaging_session::send_packet, this, placeholders::_1)));
	}

#ifndef __USE_TYPE_CONTAINER__
	void messaging_session::send_files(shared_ptr<json::value> message)
#else
	void messaging_session::send_files(shared_ptr<container::value_container> message)
#endif
	{
		if (message == nullptr)
		{
			return;
		}

		if (_session_type != session_types::file_line)
		{
			return;
		}

#ifndef __USE_TYPE_CONTAINER__
		if (_target_id != (*message)[L"header"][L"source_id"].as_string() && _target_sub_id != (*message)[L"header"][L"source_sub_id"].as_string())
		{
			return;
		}

		if ((*message)[L"header"][L"source_id"].is_null())
		{
			(*message)[L"header"][L"source_id"] = json::value::string(_source_id);
			(*message)[L"header"][L"source_sub_id"] = json::value::string(_source_sub_id);
		}

		auto& files = (*message)[L"data"][L"files"].as_array();
		for (int index = 0; index < files.size(); ++index)
		{
			shared_ptr<json::value> container = make_shared<json::value>(json::value::object(true));

			(*container)[L"header"][L"source_id"] = (*message)[L"header"][L"target_id"];
			(*container)[L"header"][L"source_sub_id"] = (*message)[L"header"][L"target_sub_id"];
			(*container)[L"header"][L"target_id"] = (*message)[L"header"][L"gateway_source_id"];
			(*container)[L"header"][L"target_sub_id"] = (*message)[L"header"][L"gateway_source_sub_id"];
			(*container)[L"header"][L"message_type"] = json::value::string(L"request_file");

			(*container)[L"data"][L"indication_id"] = (*message)[L"data"][L"indication_id"];
			(*container)[L"data"][L"source"] = files[index][L"source"];
			(*container)[L"data"][L"target"] = files[index][L"target"];

			_thread_pool->push(make_shared<job>(priorities::low, converter::to_array(container->serialize()), bind(&messaging_session::load_file_packet, this, placeholders::_1)));
		}
#else
		if (_target_id != message->source_id() && _target_sub_id != message->source_sub_id())
		{
			return;
		}

		if (message->source_id().empty())
		{
			message->set_source(_source_id, _source_sub_id);
		}

		shared_ptr<container::value_container> container = message->copy(false);
		container->swap_header();
		container->set_target(message->get_value(L"gateway_source_id")->to_string(), message->get_value(L"gateway_source_sub_id")->to_string());
		container->set_message_type(L"request_file");

		vector<shared_ptr<container::value>> files = message->value_array(L"file");
		for (auto& file : files)
		{
			container << make_shared<container::string_value>(L"indication_id", message->get_value(L"indication_id")->to_string());
			container << make_shared<container::string_value>(L"source", (*file)[L"source"]->to_string());
			container << make_shared<container::string_value>(L"target", (*file)[L"target"]->to_string());

			_thread_pool->push(make_shared<job>(priorities::low, container->serialize_array(), bind(&messaging_session::load_file_packet, this, placeholders::_1)));
			container->clear_value();
		}
#endif
	}

	void messaging_session::send_binary(const wstring target_id, const wstring& target_sub_id, const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return;
		}

		if (_session_type != session_types::binary_line)
		{
			return;
		}

		if (!_bridge_line && target_id != _target_id)
		{
			return;
		}

		if (!_bridge_line && !target_sub_id.empty() && target_id != _target_sub_id)
		{
			return;
		}

		vector<unsigned char> result;
		append_binary_on_packet(result, converter::to_array(_source_id));
		append_binary_on_packet(result, converter::to_array(_source_sub_id));
		append_binary_on_packet(result, converter::to_array(target_id));
		append_binary_on_packet(result, converter::to_array(target_sub_id));
		append_binary_on_packet(result, data);

		if (_compress_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::normal, result, bind(&messaging_session::compress_binary_packet, this, placeholders::_1)));

			return;
		}

		if (_encrypt_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::normal, result, bind(&messaging_session::encrypt_binary_packet, this, placeholders::_1)));

			return;
		}

		_thread_pool->push(make_shared<job>(priorities::top, result, bind(&messaging_session::send_binary_packet, this, placeholders::_1)));
	}

	void messaging_session::send_binary(const wstring source_id, const wstring& source_sub_id, const wstring target_id, const wstring& target_sub_id, const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return;
		}

		if (_session_type != session_types::binary_line)
		{
			return;
		}

		if (!_bridge_line && target_id != _target_id)
		{
			return;
		}

		if (!_bridge_line && !target_sub_id.empty() && target_id != _target_sub_id)
		{
			return;
		}

		vector<unsigned char> result;
		append_binary_on_packet(result, converter::to_array(source_id));
		append_binary_on_packet(result, converter::to_array(source_sub_id));
		append_binary_on_packet(result, converter::to_array(target_id));
		append_binary_on_packet(result, converter::to_array(target_sub_id));
		append_binary_on_packet(result, data);

		if (_compress_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::normal, result, bind(&messaging_session::compress_binary_packet, this, placeholders::_1)));

			return;
		}

		if (_encrypt_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::normal, result, bind(&messaging_session::encrypt_binary_packet, this, placeholders::_1)));

			return;
		}

		_thread_pool->push(make_shared<job>(priorities::top, result, bind(&messaging_session::send_binary_packet, this, placeholders::_1)));
	}

	void messaging_session::receive_on_tcp(const data_modes& data_mode, const vector<unsigned char>& data)
	{
		switch (data_mode)
		{
		case data_modes::packet_mode:
			_thread_pool->push(make_shared<job>(priorities::high, data, bind(&messaging_session::decrypt_packet, this, placeholders::_1)));
			break;
		case data_modes::file_mode:
			_thread_pool->push(make_shared<job>(priorities::high, data, bind(&messaging_session::decrypt_file_packet, this, placeholders::_1)));
			break;
		}

	}

	void messaging_session::disconnected(void)
	{
		stop();

		if (_connection)
		{
			_connection(get_ptr(), false);
		}
	}

	bool messaging_session::check_confirm_condition(void)
	{
		this_thread::sleep_for(chrono::seconds(1));

		if (_confirm == session_conditions::confirmed)
		{
			return true;
		}

		_confirm = session_conditions::expired;

		return true;
	}

	bool messaging_session::contained_snipping_target(const wstring& snipping_target)
	{
		auto target = find(_snipping_targets.begin(), _snipping_targets.end(), snipping_target);
		if (target == _snipping_targets.end())
		{
			return false;
		}

		return true;
	}

	bool messaging_session::compress_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		if (_encrypt_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::normal, compressor::compression(data), bind(&messaging_session::encrypt_packet, this, placeholders::_1)));

			return true;
		}

		_thread_pool->push(make_shared<job>(priorities::top, compressor::compression(data), bind(&messaging_session::send_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::encrypt_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		_thread_pool->push(make_shared<job>(priorities::top, encryptor::encryption(data, _key, _iv), bind(&messaging_session::send_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::send_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		return send_on_tcp(_socket, data_modes::packet_mode, data);
	}

	bool messaging_session::decompress_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		if (_compress_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::normal, compressor::decompression(data), bind(&messaging_session::receive_packet, this, placeholders::_1)));

			return true;
		}

		_thread_pool->push(make_shared<job>(priorities::high, data, bind(&messaging_session::receive_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::decrypt_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		if (_encrypt_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::high, encryptor::decryption(data, _key, _iv), bind(&messaging_session::decompress_packet, this, placeholders::_1)));

			return true;
		}

		_thread_pool->push(make_shared<job>(priorities::high, data, bind(&messaging_session::decompress_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::receive_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

#ifndef __USE_TYPE_CONTAINER__
		shared_ptr<json::value> message = make_shared<json::value>(json::value::parse(converter::to_wstring(data)));
#else
		shared_ptr<container::value_container> message = make_shared<container::value_container>(data, true);
#endif
		if (message == nullptr)
		{
			return false;
		}

		logger::handle().write(logging_level::packet, data);

#ifndef __USE_TYPE_CONTAINER__
		auto target = _message_handlers.find((*message.get())[L"header"][L"message_type"].as_string());
#else
		auto target = _message_handlers.find(message->message_type());
#endif
		if (target == _message_handlers.end())
		{
			return normal_message(message);
		}

		return target->second(message);
	}

	bool messaging_session::load_file_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

#ifndef __USE_TYPE_CONTAINER__
		auto temp = converter::to_wstring(data);
		shared_ptr<json::value> message = make_shared<json::value>(json::value::parse(converter::to_wstring(data)));
#else
		shared_ptr<container::value_container> message = make_shared<container::value_container>(data);
#endif
		if (message == nullptr)
		{
			return false;
		}

		vector<unsigned char> result;
#ifndef __USE_TYPE_CONTAINER__
		append_binary_on_packet(result, converter::to_array((*message)[L"data"][L"indication_id"].as_string()));
		append_binary_on_packet(result, converter::to_array((*message)[L"header"][L"source_id"].as_string()));
		append_binary_on_packet(result, converter::to_array((*message)[L"header"][L"source_sub_id"].as_string()));
		append_binary_on_packet(result, converter::to_array((*message)[L"header"][L"target_id"].as_string()));
		append_binary_on_packet(result, converter::to_array((*message)[L"header"][L"target_sub_id"].as_string()));
		append_binary_on_packet(result, converter::to_array((*message)[L"data"][L"source"].as_string()));
		append_binary_on_packet(result, converter::to_array((*message)[L"data"][L"target"].as_string()));
		append_binary_on_packet(result, file::load((*message)[L"data"][L"source"].as_string()));
#else
		append_binary_on_packet(result, converter::to_array(message->get_value(L"indication_id")->to_string()));
		append_binary_on_packet(result, converter::to_array(message->source_id()));
		append_binary_on_packet(result, converter::to_array(message->source_sub_id()));
		append_binary_on_packet(result, converter::to_array(message->target_id()));
		append_binary_on_packet(result, converter::to_array(message->target_sub_id()));
		append_binary_on_packet(result, converter::to_array(message->get_value(L"source")->to_string()));
		append_binary_on_packet(result, converter::to_array(message->get_value(L"target")->to_string()));
		append_binary_on_packet(result, file::load(message->get_value(L"source")->to_string()));
#endif

		if (_compress_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::normal, result, bind(&messaging_session::compress_file_packet, this, placeholders::_1)));

			return true;
		}

		if (_encrypt_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::normal, result, bind(&messaging_session::encrypt_file_packet, this, placeholders::_1)));

			return true;
		}

		_thread_pool->push(make_shared<job>(priorities::top, result, bind(&messaging_session::send_file_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::compress_file_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		if (_encrypt_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::high, compressor::compression(data), bind(&messaging_session::encrypt_file_packet, this, placeholders::_1)));

			return true;
		}

		_thread_pool->push(make_shared<job>(priorities::top, compressor::compression(data), bind(&messaging_session::send_file_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::encrypt_file_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		_thread_pool->push(make_shared<job>(priorities::top, encryptor::encryption(data, _key, _iv), bind(&messaging_session::send_file_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::send_file_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		return send_on_tcp(_socket, data_modes::file_mode, data);
	}

	bool messaging_session::decompress_file_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		if (_compress_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::low, compressor::decompression(data), bind(&messaging_session::receive_file_packet, this, placeholders::_1)));

			return true;
		}

		_thread_pool->push(make_shared<job>(priorities::low , data, bind(&messaging_session::receive_file_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::decrypt_file_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		if (_encrypt_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::normal, encryptor::decryption(data, _key, _iv), bind(&messaging_session::decompress_file_packet, this, placeholders::_1)));

			return true;
		}

		_thread_pool->push(make_shared<job>(priorities::normal, data, bind(&messaging_session::decompress_file_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::receive_file_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		size_t index = 0;
		wstring indication_id = converter::to_wstring(devide_binary_on_packet(data, index));
		wstring source_id = converter::to_wstring(devide_binary_on_packet(data, index));
		wstring source_sub_id = converter::to_wstring(devide_binary_on_packet(data, index));
		wstring target_id = converter::to_wstring(devide_binary_on_packet(data, index));
		wstring target_sub_id = converter::to_wstring(devide_binary_on_packet(data, index));
		wstring source_path = converter::to_wstring(devide_binary_on_packet(data, index));
		wstring target_path = converter::to_wstring(devide_binary_on_packet(data, index));

		vector<unsigned char> result;
		append_binary_on_packet(result, converter::to_array(indication_id));
		append_binary_on_packet(result, converter::to_array(target_id));
		append_binary_on_packet(result, converter::to_array(target_sub_id));
		if (file::save(target_path, devide_binary_on_packet(data, index)))
		{
			append_binary_on_packet(result, converter::to_array(target_path));
		}
		else
		{
			append_binary_on_packet(result, converter::to_array(L""));
		}

		_thread_pool->push(make_shared<job>(priorities::high, result, bind(&messaging_session::notify_file_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::notify_file_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		size_t index = 0;
		wstring indication_id = converter::to_wstring(devide_binary_on_packet(data, index));
		wstring target_id = converter::to_wstring(devide_binary_on_packet(data, index));
		wstring target_sub_id = converter::to_wstring(devide_binary_on_packet(data, index));
		wstring target_path = converter::to_wstring(devide_binary_on_packet(data, index));

		if (_received_file)
		{
			_received_file(target_id, target_sub_id, indication_id, target_path);
		}

		return true;
	}

	bool messaging_session::compress_binary_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		if (_encrypt_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::normal, compressor::compression(data), bind(&messaging_session::encrypt_binary_packet, this, placeholders::_1)));

			return true;
		}

		_thread_pool->push(make_shared<job>(priorities::top, compressor::compression(data), bind(&messaging_session::send_binary_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::encrypt_binary_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		_thread_pool->push(make_shared<job>(priorities::top, encryptor::encryption(data, _key, _iv), bind(&messaging_session::send_binary_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::send_binary_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		return send_on_tcp(_socket, data_modes::binary_mode, data);
	}

	bool messaging_session::decompress_binary_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		if (_compress_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::normal, compressor::decompression(data), bind(&messaging_session::receive_binary_packet, this, placeholders::_1)));

			return true;
		}

		_thread_pool->push(make_shared<job>(priorities::high, data, bind(&messaging_session::receive_binary_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::decrypt_binary_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		if (_encrypt_mode)
		{
			_thread_pool->push(make_shared<job>(priorities::high, encryptor::decryption(data, _key, _iv), bind(&messaging_session::decompress_binary_packet, this, placeholders::_1)));

			return true;
		}

		_thread_pool->push(make_shared<job>(priorities::high, data, bind(&messaging_session::decompress_binary_packet, this, placeholders::_1)));

		return true;
	}

	bool messaging_session::receive_binary_packet(const vector<unsigned char>& data)
	{
		if (data.empty())
		{
			return false;
		}

		size_t index = 0;
		wstring source_id = converter::to_wstring(devide_binary_on_packet(data, index));
		wstring source_sub_id = converter::to_wstring(devide_binary_on_packet(data, index));
		wstring target_id = converter::to_wstring(devide_binary_on_packet(data, index));
		wstring target_sub_id = converter::to_wstring(devide_binary_on_packet(data, index));
		vector<unsigned char> target_data = devide_binary_on_packet(data, index);
		if (_received_data)
		{
			_received_data(source_id, source_sub_id, target_id, target_sub_id, target_data);
		}

		return true;
	}

#ifndef __USE_TYPE_CONTAINER__
	bool messaging_session::normal_message(shared_ptr<json::value> message)
#else
	bool messaging_session::normal_message(shared_ptr<container::value_container> message)
#endif
	{
		if (message == nullptr)
		{
			return false;
		}

		if (_confirm != session_conditions::confirmed)
		{
			return false;
		}

		if (_received_message)
		{
			_received_message(message);
		}

		return true;
	}

#ifndef __USE_TYPE_CONTAINER__
	bool messaging_session::connection_message(shared_ptr<json::value> message)
#else
	bool messaging_session::connection_message(shared_ptr<container::value_container> message)
#endif
	{
		if (message == nullptr)
		{
			return false;
		}

#ifndef __USE_TYPE_CONTAINER__
		_target_id = (*message)[L"header"][L"source_id"].as_string();
		_session_type = (session_types)(*message)[L"data"][L"session_type"].as_integer();
#else
		_target_id = message->source_id();
		_session_type = (session_types)message->get_value(L"session_type")->to_short();
#endif


		auto iter = find_if(_possible_session_types.begin(), _possible_session_types.end(), 
			[&](const session_types& type) 
			{
				return type == _session_type;
			});
		if (iter == _possible_session_types.end())
		{
			_confirm = session_conditions::expired;
			logger::handle().write(logging_level::error, L"expired this line = \"cannot accept unknown session type\"");

			return false;
		}

		if (_source_id == _target_id)
		{
			_confirm = session_conditions::expired;
			logger::handle().write(logging_level::error, L"expired this line = \"cannot use same id with server\"");

			return false;
		}

		if (!_ignore_target_ids.empty())
		{
			auto target = find(_ignore_target_ids.begin(), _ignore_target_ids.end(), _target_id);
			if (target != _ignore_target_ids.end())
			{
				_confirm = session_conditions::expired;
				logger::handle().write(logging_level::error, L"expired this line = \"cannot connect with ignored id on server\"");

				return false;
			}
		}

		if (_kill_code)
		{
			_confirm = session_conditions::expired;
			logger::handle().write(logging_level::error, L"expired this line = \"set kill code\"");

			return false;
		}

		// check connection key
#ifndef __USE_TYPE_CONTAINER__
		if (!same_key_check((*message)[L"data"][L"connection_key"]))
#else
		if (!same_key_check(message->get_value(L"connection_key")))
#endif
		{
			_confirm = session_conditions::expired;

			return false;
		}

		// compare both session id an client id
		if (!same_id_check())
		{
			_confirm = session_conditions::expired;

			return false;
		}

		_confirm = session_conditions::confirmed;

		// check snipping target list
#ifndef __USE_TYPE_CONTAINER__
		shared_ptr<json::value> container = make_shared<json::value>(json::value::object(true));

		(*container)[L"header"][L"source_id"] = json::value::string(_source_id);
		(*container)[L"header"][L"source_sub_id"] = json::value::string(_source_sub_id);
		(*container)[L"header"][L"target_id"] = json::value::string(_target_id);
		(*container)[L"header"][L"target_sub_id"] = json::value::string(_target_sub_id);
		(*container)[L"header"][L"message_type"] = json::value::string(L"confirm_connection");

		_snipping_targets.clear();

		int index2 = 0;
		(*container)[L"data"][L"snipping_targets"] = json::value::array();
		auto& snipping_targets = (*message)[L"data"][L"snipping_targets"].as_array();
		for (int index = 0; index < snipping_targets.size(); ++index)
		{
			auto target = find(_ignore_snipping_targets.begin(), _ignore_snipping_targets.end(), snipping_targets[index].as_string());
			if (target != _ignore_snipping_targets.end())
			{
				continue;
			}

			_snipping_targets.push_back(snipping_targets[index].as_string());

			(*container)[L"data"][L"snipping_targets"][index2++] = json::value::string(snipping_targets[index].as_string());
		}

		generate_key();

		(*container)[L"data"][L"confirm"] = json::value::boolean(true);
		(*container)[L"data"][L"key"] = json::value::string(_target_sub_id);
		(*container)[L"data"][L"iv"] = json::value::string(_target_sub_id);
		(*container)[L"data"][L"encrypt_mode"] = json::value::boolean(_encrypt_mode);
#else
		shared_ptr<value> acceptable_snipping_targets = make_shared<container::container_value>(L"snipping_targets");

		_snipping_targets.clear();
		vector<shared_ptr<value>> snipping_targets = message->get_value(L"snipping_targets")->children();
		for (auto& snipping_target : snipping_targets)
		{
			if (snipping_target == nullptr)
			{
				continue;
			}

			if (snipping_target->name() != L"snipping_target")
			{
				continue;
			}

			auto target = find(_ignore_snipping_targets.begin(), _ignore_snipping_targets.end(), snipping_target->to_string());
			if (target != _ignore_snipping_targets.end())
			{
				continue;
			}

			_snipping_targets.push_back(snipping_target->to_string());

			acceptable_snipping_targets->add(make_shared<container::string_value>(L"snipping_target", snipping_target->to_string()));
		}

		generate_key();

		shared_ptr<container::value_container> container = make_shared<container::value_container>(_source_id, _source_sub_id, _target_id, _target_sub_id, L"confirm_connection",
			vector<shared_ptr<container::value>> {
			make_shared<container::bool_value>(L"confirm", true),
				make_shared<container::string_value>(L"key", _key),
				make_shared<container::string_value>(L"iv", _iv),
				make_shared<container::bool_value>(L"encrypt_mode", _encrypt_mode),
				acceptable_snipping_targets
		});
#endif

		if (_compress_mode)
		{
#ifndef __USE_TYPE_CONTAINER__
			_thread_pool->push(make_shared<job>(priorities::high, compressor::compression(converter::to_array(container->serialize())), bind(&messaging_session::send_packet, this, placeholders::_1)));
#else
			_thread_pool->push(make_shared<job>(priorities::high, compressor::compression(container->serialize_array()), bind(&messaging_session::send_packet, this, placeholders::_1)));
#endif

			if (_connection)
			{
				_connection(get_ptr(), true);
			}

			return true;
		}

#ifndef __USE_TYPE_CONTAINER__
		_thread_pool->push(make_shared<job>(priorities::top, converter::to_array(container->serialize()), bind(&messaging_session::send_packet, this, placeholders::_1)));
#else
		_thread_pool->push(make_shared<job>(priorities::top, container->serialize_array(), bind(&messaging_session::send_packet, this, placeholders::_1)));
#endif

		if (_connection)
		{
			_connection(get_ptr(), true);
		}

		return true;
	}

#ifndef __USE_TYPE_CONTAINER__
	bool messaging_session::request_files(shared_ptr<json::value> message)
#else
	bool messaging_session::request_files(shared_ptr<container::value_container> message)
#endif
	{
		if (message == nullptr)
		{
			return false;
		}

		if (_confirm != session_conditions::confirmed)
		{
			return false;
		}

#ifndef __USE_TYPE_CONTAINER__
		auto& files = (*message)[L"data"][L"files"].as_array();
		for (int index = 0; index < files.size(); ++index)
		{
			shared_ptr<json::value> container = make_shared<json::value>(json::value::object(true));

			(*container)[L"header"][L"source_id"] = (*message)[L"header"][L"target_id"];
			(*container)[L"header"][L"source_sub_id"] = (*message)[L"header"][L"target_sub_id"];
			(*container)[L"header"][L"target_id"] = (*message)[L"header"][L"source_id"];
			(*container)[L"header"][L"target_sub_id"] = (*message)[L"header"][L"source_sub_id"];
			(*container)[L"header"][L"message_type"] = json::value::string(L"request_file");

			(*container)[L"data"][L"indication_id"] = (*message)[L"data"][L"indication_id"];
			(*container)[L"data"][L"source"] = files[index][L"source"];
			(*container)[L"data"][L"target"] = files[index][L"target"];

			_thread_pool->push(make_shared<job>(priorities::low, converter::to_array(container->serialize()), bind(&messaging_session::load_file_packet, this, placeholders::_1)));
		}
#else
		shared_ptr<container::value_container> container = message->copy(false);
		container->swap_header();
		container->set_message_type(L"request_file");

		vector<shared_ptr<container::value>> files = message->value_array(L"file");
		for (auto& file : files)
		{
			container << make_shared<container::string_value>(L"indication_id", message->get_value(L"indication_id")->to_string());
			container << make_shared<container::string_value>(L"source", (*file)[L"source"]->to_string());
			container << make_shared<container::string_value>(L"target", (*file)[L"target"]->to_string());

			_thread_pool->push(make_shared<job>(priorities::low, container->serialize_array(), bind(&messaging_session::load_file_packet, this, placeholders::_1)));
			container->clear_value();
		}
#endif

		return true;
	}

#ifndef __USE_TYPE_CONTAINER__
	bool messaging_session::echo_message(shared_ptr<json::value> message)
#else
	bool messaging_session::echo_message(shared_ptr<container::value_container> message)
#endif
	{
		if (message == nullptr)
		{
			return false;
		}

		if (_confirm != session_conditions::confirmed)
		{
			return false;
		}

#ifndef __USE_TYPE_CONTAINER__
		if (!(*message)[L"data"][L"response"].is_null())
		{
			logger::handle().write(logging_level::information, fmt::format(L"received echo: {}", message->serialize()));

			return true;
		}

		shared_ptr<json::value> container = make_shared<json::value>(json::value::object(true));

		(*container)[L"header"][L"source_id"] = (*message)[L"header"][L"target_id"];
		(*container)[L"header"][L"source_sub_id"] = (*message)[L"header"][L"target_sub_id"];
		(*container)[L"header"][L"target_id"] = (*message)[L"header"][L"source_id"];
		(*container)[L"header"][L"target_sub_id"] = (*message)[L"header"][L"source_sub_id"];
		(*container)[L"header"][L"message_type"] = (*message)[L"header"][L"message_type"];

		(*container)[L"data"] = (*message)[L"data"];
		(*container)[L"data"][L"response"] = json::value::boolean(true);

		_thread_pool->push(make_shared<job>(priorities::low, converter::to_array(container->serialize()), bind(&messaging_session::send_packet, this, placeholders::_1)));
#else
		vector<shared_ptr<value>> response = (*message)[L"response"];
		if (!response.empty())
		{
			logger::handle().write(logging_level::information, fmt::format(L"received echo: {}", message->serialize()));

			return true;
		}

		message->swap_header();

		message << make_shared<bool_value>(L"response", true);

		_thread_pool->push(make_shared<job>(priorities::top, message->serialize_array(), bind(&messaging_session::send_packet, this, placeholders::_1)));
#endif

		return true;
	}

	void messaging_session::generate_key(void)
	{
		if (!_encrypt_mode)
		{
			_key = L"";
			_iv = L"";

			return;
		}
		
		auto encrypt_key = encryptor::create_key();
		_key = encrypt_key.first;
		_iv = encrypt_key.second;
	}

#ifndef __USE_TYPE_CONTAINER__
	bool messaging_session::same_key_check(const json::value& key)
#else
	bool messaging_session::same_key_check(shared_ptr<container::value> key)
#endif
	{
#ifndef __USE_TYPE_CONTAINER__
		if (!key.is_null() && _connection_key == key.as_string())
#else
		if (key != nullptr && _connection_key == key->to_string())
#endif
		{
			return true;
		}

		logger::handle().write(logging_level::information, L"ignored this line = \"unknown connection key\"");

#ifndef __USE_TYPE_CONTAINER__
		shared_ptr<json::value> container = make_shared<json::value>(json::value::object(true));
#else
		shared_ptr<container::value_container> container = make_shared<container::value_container>(_source_id, _source_sub_id, _target_id, _target_sub_id, L"confirm_connection",
			vector<shared_ptr<container::value>> {
			make_shared<container::bool_value>(L"confirm", false),
				make_shared<container::string_value>(L"reason", L"ignored this line = \"unknown connection key\"")
		});
#endif

		send(container);

		return false;
	}

	bool messaging_session::same_id_check(void)
	{
		if (_target_id != _source_id)
		{
			return true;
		}

		logger::handle().write(logging_level::information, L"ignored this line = \"cannot use same id with server\"");

#ifndef __USE_TYPE_CONTAINER__
		shared_ptr<json::value> container = make_shared<json::value>(json::value::object(true));
#else
		shared_ptr<container::value_container> container = make_shared<container::value_container>(_source_id, _source_sub_id, _target_id, _target_sub_id, L"confirm_connection",
			vector<shared_ptr<container::value>> {
				make_shared<container::bool_value>(L"confirm", false),
				make_shared<container::string_value>(L"reason", L"ignored this line = \"cannot use same id with server\"")
		});
#endif

		send(container);

		return false;
	}
}
