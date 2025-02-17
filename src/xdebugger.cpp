/***************************************************************************
* Copyright (c) 2018, Martin Renou, Johan Mabille, Sylvain Corlay, and     *
* Wolf Vollprecht                                                          *
* Copyright (c) 2018, QuantStack                                           *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

// This must be included BEFORE pybind
// otherwise it fails to build on Windows
// because of the redefinition of snprintf
#include "nlohmann/json.hpp"

#include "pybind11_json/pybind11_json.hpp"

#include "pybind11/pybind11.h"
#include "pybind11/stl.h"

#include "xeus/xinterpreter.hpp"
#include "xeus/xmiddleware.hpp"
#include "xeus/xsystem.hpp"

#include "xeus-python/xdebugger.hpp"
#include "xeus-python/xutils.hpp"
#include "xdebugpy_client.hpp"
#include "xinternal_utils.hpp"

namespace nl = nlohmann;
namespace py = pybind11;

using namespace pybind11::literals;
using namespace std::placeholders;

namespace xpyt
{
    debugger::debugger(zmq::context_t& context,
                       const xeus::xconfiguration& config,
                       const std::string& user_name,
                       const std::string& session_id,
                       const nl::json& debugger_config)
        : xdebugger_base(context)
        , p_debugpy_client(new xdebugpy_client(context,
                                               config,
                                               xeus::get_socket_linger(),
                                               xdap_tcp_configuration(xeus::dap_tcp_type::client,
                                                                      xeus::dap_init_type::parallel,
                                                                      user_name,
                                                                      session_id),
                                               get_event_callback()))
        , m_debugpy_host("127.0.0.1")
        , m_debugpy_port("")
        , m_debugger_config(debugger_config)
    {
        m_debugpy_port = xeus::find_free_port(100, 5678, 5900);
        register_request_handler("inspectVariables", std::bind(&debugger::inspect_variables_request, this, _1), false);
        register_request_handler("richInspectVariables", std::bind(&debugger::rich_inspect_variables_request, this, _1), false);
        register_request_handler("attach", std::bind(&debugger::attach_request, this, _1), true);
        register_request_handler("configurationDone", std::bind(&debugger::configuration_done_request, this, _1), true);
    }

    debugger::~debugger()
    {
        delete p_debugpy_client;
        p_debugpy_client = nullptr;
    }

    namespace
    {
        using name_list = std::vector<std::string>;

        const name_list& get_excluded_variables()
        {
            static name_list l =
            { 
                "__name__",
                "__doc__",
                "__package__",
                "__loader__",
                "__spec__",
                "__annotations__",
                "__builtins__",
                "__builtin__",
                "display",
                "get_ipython",
                "debugpy",
                "exit",
                "quit",
                "In",
                "Out",
                "_oh",
                "_dh",
                "_",
                "__",
                "___"
            };
            return l;
        };

        bool keep_variable(const std::string& var_name)
        {
            const name_list& l = get_excluded_variables();
            bool res = var_name.substr(0u, 2u) != "_i";
            res = res && !(var_name[0] == '_' && std::isdigit(var_name[1]));
            res = res && std::find(l.cbegin(), l.cend(), var_name) == l.cend();
            return res;
        }
    }

    nl::json debugger::inspect_variables_request(const nl::json& message)
    {
        py::gil_scoped_acquire acquire;
        py::object variables = py::globals();

        nl::json json_vars = nl::json::array();
        for (const py::handle& key : variables)
        {
            nl::json json_var = nl::json::object();
            std::string var_name = py::str(key);
            if (keep_variable(var_name))
            {
                json_var["name"] = var_name;
                json_var["variablesReference"] = 0;
                try
                {
                    json_var["value"] = variables[key];
                }
                catch(std::exception&)
                {
                    json_var["value"] = py::repr(variables[key]);
                }
                std::string var_type = py::str(variables[key].get_type());
                size_t size = var_type.size();
                std::string var_trunc_type = var_type.substr(size_t(8), size - 10u);
                json_var["type"] = var_trunc_type;
                json_vars.push_back(json_var);
            }
        }

        nl::json reply = {
            {"type", "response"},
            {"request_seq", message["seq"]},
            {"success", true},
            {"command", message["command"]},
            {"body", {
                {"variables", json_vars}
            }}
        };

        return reply;
    }

    nl::json debugger::rich_inspect_variables_request(const nl::json& message)
    {


        std::string var_name = message["arguments"]["variableName"].get<std::string>();
        std::string var_repr_data = var_name + "_repr_data";
        std::string var_repr_metadata = var_name + "_repr_metada";

        if (base_type::get_stopped_threads().empty())
        {
            // The code did not hit a breakpoint, we use the interpreter 
            // to get the rich reprensentation of the variable
            std::string code = "from IPython import get_ipython;";
            code += var_repr_data + ',' + var_repr_metadata + "= get_ipython().display_formatter.format(" + var_name + ")";
            py::gil_scoped_acquire acquire;
            exec(py::str(code));
        }
        else
        {   
            // The code has stopped on a breakpoint, we use the setExpression request
            // to get the rich representation of the variable
            std::string lvalue = var_repr_data + ',' + var_repr_metadata;
            std::string code = "get_ipython().display_formatter.format(" + var_name + ")";
            int frame_id = message["arguments"]["frameId"].get<int>();
            int seq = message["seq"].get<int>();
            nl::json request = {
                {"type", "request"},
                {"command", "setExpression"},
                {"seq", seq+1},
                {"arguments", {
                    {"expression", lvalue},
                    {"value", code},
                    {"frameId", frame_id}
                }}
            };
            forward_message(request);
        }

        nl::json reply = {
            {"type", "response"},
            {"request_seq", message["seq"]},
            {"success", false},
            {"command", message["command"]}
        };

        py::gil_scoped_acquire acquire;
        py::object variables = py::globals();
        py::object repr_data = variables[py::str(var_repr_data)];
        py::object repr_metadata = variables[py::str(var_repr_metadata)];
        nl::json body = {
            {"data", {}},
            {"metadata", {}}
        };
        for (const py::handle& key : repr_data)
        {
            std::string data_key = py::str(key);
            body["data"][data_key] = repr_data[key];
            if (repr_metadata.contains(key))
            {
                body["metadata"][data_key] = repr_metadata[key];
            }
        }
        reply["body"] = body;
        reply["success"] = true;
        return reply;
    }

    nl::json debugger::attach_request(const nl::json& message)
    {
        nl::json new_message = message;
        new_message["arguments"]["connect"] = {
            {"host", m_debugpy_host},
            {"port", std::stoi(m_debugpy_port)}
        };
        new_message["arguments"]["logToFile"] = true;
        return forward_message(new_message);
    }

    nl::json debugger::configuration_done_request(const nl::json& message)
    {
        int seq = message["seq"].get<int>();
        nl::json reply = {
            {"seq", seq},
            {"type", "response"},
            {"request_seq", message["seq"]},
            {"success", true},
            {"command", message["command"]}
        };
        return reply;
    }

    bool debugger::start_debugpy()
    {
        // import debugpy
        std::string code = "import debugpy;";
        // specify sys.executable
        if (std::getenv("XEUS_LOG") != nullptr)
        {
            std::ofstream out("xeus.log", std::ios_base::app);
            out << "===== DEBUGGER CONFIG =====" << std::endl;
            out << m_debugger_config.dump() << std::endl;
        }
        auto it = m_debugger_config.find("python");
        if (it != m_debugger_config.end())
        {
            code += "debugpy.configure({\'python\': r\'" + it->template get<std::string>()  + "\'});";
        }
        // call to debugpy.listen
        code += "debugpy.listen((\'" + m_debugpy_host + "\'," + m_debugpy_port + "))";
        nl::json json_code;
        json_code["code"] = code;
        nl::json rep = xdebugger::get_control_messenger().send_to_shell(json_code);
        std::string status = rep["status"].get<std::string>();
        if(status != "ok")
        {
            std::string ename = rep["ename"].get<std::string>();
            std::string evalue = rep["evalue"].get<std::string>();
            std::vector<std::string> traceback = rep["traceback"].get<std::vector<std::string>>();
            std::clog << "Exception raised when trying to import debugpy" << std::endl;
            for(std::size_t i = 0; i < traceback.size(); ++i)
            {
                std::clog << traceback[i] << std::endl;
            }
            std::clog << ename << " - " << evalue << std::endl;
        }
        return status == "ok";
    }

    bool debugger::start(zmq::socket_t& header_socket, zmq::socket_t& request_socket)
    {
        std::string temp_dir = xeus::get_temp_directory_path();
        std::string log_dir = temp_dir + "/" + "xpython_debug_logs_" + std::to_string(xeus::get_current_pid());

        xeus::create_directory(log_dir);

        static bool debugpy_started = start_debugpy();
        if (!debugpy_started)
        {
            return false;
        }

        std::string controller_end_point = xeus::get_controller_end_point("debugger");
        std::string controller_header_end_point = xeus::get_controller_end_point("debugger_header");
        std::string publisher_end_point = xeus::get_publisher_end_point();

        request_socket.bind(controller_end_point);
        header_socket.bind(controller_header_end_point);

        std::string debugpy_end_point = "tcp://" + m_debugpy_host + ':' + m_debugpy_port;
        std::thread client(&xdap_tcp_client::start_debugger,
                           p_debugpy_client,
                           debugpy_end_point,
                           publisher_end_point,
                           controller_end_point,
                           controller_header_end_point);
        client.detach();

        request_socket.send(zmq::message_t("REQ", 3), zmq::send_flags::none);
        zmq::message_t ack;
        (void)request_socket.recv(ack);

        std::string tmp_folder =  get_tmp_prefix();
        xeus::create_directory(tmp_folder);

        return true;
    }

    void debugger::stop(zmq::socket_t& header_socket, zmq::socket_t& request_socket)
    {
        std::string controller_end_point = xeus::get_controller_end_point("debugger");
        std::string controller_header_end_point = xeus::get_controller_end_point("debugger_header");
        request_socket.unbind(controller_end_point);
        header_socket.unbind(controller_header_end_point);
    }

    xeus::xdebugger_info debugger::get_debugger_info() const
    {
        return xeus::xdebugger_info(xeus::get_tmp_hash_seed(),
                                    get_tmp_prefix(),
                                    get_tmp_suffix(),
                                    true,
                                    {"Python Exceptions"});
    }

    std::string debugger::get_cell_temporary_file(const std::string& code) const
    {
        return get_cell_tmp_file(code);
    }

    std::unique_ptr<xeus::xdebugger> make_python_debugger(zmq::context_t& context,
                                                          const xeus::xconfiguration& config,
                                                          const std::string& user_name,
                                                          const std::string& session_id,
                                                          const nl::json& debugger_config)
    {
        return std::unique_ptr<xeus::xdebugger>(new debugger(context, config, user_name, session_id, debugger_config));
    }
}
