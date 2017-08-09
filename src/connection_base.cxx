/*-------------------------------------------------------------------------
 *
 *   FILE
 *	connection_base.cxx
 *
 *   DESCRIPTION
 *      implementation of the pqxx::connection_base abstract base class.
 *   pqxx::connection_base encapsulates a frontend to backend connection
 *
 * Copyright (c) 2001-2017, Jeroen T. Vermeulen <jtv@xs4all.nl>
 *
 * See COPYING for copyright license.  If you did not receive a file called
 * COPYING with this source code, please notify the distributor of this mistake,
 * or contact the author.
 *
 *-------------------------------------------------------------------------
 */
#include "pqxx/compiler-internal.hxx"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <stdexcept>

#ifdef PQXX_HAVE_SYS_SELECT_H
#include <sys/select.h>
#else
#include <sys/types.h>
#if defined(_WIN32)
#include <winsock2.h>
#endif
#if defined(HAVE_UNISTD_H)
#include <unistd.h>
#endif
#endif // PQXX_HAVE_SYS_SELECT_H

#ifdef PQXX_HAVE_POLL
#include <poll.h>
#endif

#include "libpq-fe.h"

#include "pqxx/binarystring"
#include "pqxx/connection"
#include "pqxx/connection_base"
#include "pqxx/nontransaction"
#include "pqxx/pipeline"
#include "pqxx/result"
#include "pqxx/strconv"
#include "pqxx/transaction"
#include "pqxx/notification"

#include "pqxx/internal/gates/connection-reactivation_avoidance_exemption.hxx"
#include "pqxx/internal/gates/errorhandler-connection.hxx"
#include "pqxx/internal/gates/result-creation.hxx"
#include "pqxx/internal/gates/result-connection.hxx"

using namespace pqxx;
using namespace pqxx::internal;
using namespace pqxx::prepare;


namespace
{
#ifndef PQXX_HAVE_POLL
#ifdef PQXX_SELECT_ACCEPTS_NULL
  // The always-empty fd_set
fd_set *const fdset_none = nullptr;
#else	// PQXX_SELECT_ACCEPTS_NULL
fd_set emptyfd;	// Relies on zero-initialization
fd_set *const fdset_none = &emptyfd;
#endif	// PQXX_SELECT_ACCEPTS_NULL
#endif  // !PQXX_HAVE_POLL


#ifndef PQXX_HAVE_POLL
// Concentrate stupid "old-style cast" warnings for GNU libc in one place, and
// by using "C" linkage, perhaps silence them altogether.
void set_fdbit(int f, fd_set *s)
{

#ifdef _MSC_VER
#pragma warning ( push )
#pragma warning ( disable: 4389 ) // signed/unsigned mismatch
#pragma warning ( disable: 4127 ) // conditional expression is constant
#endif

  FD_SET(f, s);

#ifdef _MSC_VER
#pragma warning ( pop )
#endif

}


void clear_fdmask(fd_set *mask)
{
  FD_ZERO(mask);
}
#endif


extern "C"
{
// The PQnoticeProcessor that receives an error or warning from libpq and sends
// it to the appropriate connection for processing.
void pqxx_notice_processor(void *conn, const char *msg)
{
  reinterpret_cast<pqxx::connection_base *>(conn)->process_notice(msg);
}
} // extern "C"
} // namespace


std::string pqxx::encrypt_password(
        const std::string &user, const std::string &password)
{
  std::unique_ptr<char, void (*)(char *)> p(
	PQencryptPassword(password.c_str(), user.c_str()),
        freepqmem_templated<char>);
  return std::string(p.get());
}


pqxx::connection_base::connection_base(connectionpolicy &pol) :
  m_Conn(nullptr),
  m_policy(pol),
  m_Trans(),
  m_errorhandlers(),
  m_Trace(nullptr),
  m_serverversion(0),
  m_reactivation_avoidance(),
  m_unique_id(0),
  m_Completed(false),
  m_inhibit_reactivation(false),
  m_caps(),
  m_verbosity(normal)
{
  clearcaps();
}


void pqxx::connection_base::init()
{
  m_Conn = m_policy.do_startconnect(m_Conn);
  if (m_policy.is_ready(m_Conn)) activate();
}


pqxx::result pqxx::connection_base::make_result(
	internal::pq::PGresult *rhs,
	const std::string &query)
{
  return gate::result_creation::create(rhs, query);
}


int pqxx::connection_base::backendpid() const noexcept
{
  return m_Conn ? PQbackendPID(m_Conn) : 0;
}


namespace
{
PQXX_PURE int socket_of(const ::pqxx::internal::pq::PGconn *c) noexcept
{
  return c ? PQsocket(c) : -1;
}
}


int pqxx::connection_base::sock() const noexcept
{
  return socket_of(m_Conn);
}


void pqxx::connection_base::activate()
{
  if (!is_open())
  {
    if (m_inhibit_reactivation)
      throw broken_connection(
	"Could not reactivate connection; "
	"reactivation is inhibited");

    // If any objects were open that didn't survive the closing of our
    // connection, don't try to reactivate
    if (m_reactivation_avoidance.get()) return;

    try
    {
      m_Conn = m_policy.do_startconnect(m_Conn);
      m_Conn = m_policy.do_completeconnect(m_Conn);
      m_Completed = true;	// (But retracted if error is thrown below)

      if (!is_open()) throw broken_connection();

      SetupState();
    }
    catch (const broken_connection &e)
    {
      disconnect();
      m_Completed = false;
      throw broken_connection(e.what());
    }
    catch (const std::exception &)
    {
      m_Completed = false;
      throw;
    }
  }
}


void pqxx::connection_base::deactivate()
{
  if (!m_Conn) return;

  if (m_Trans.get())
    throw usage_error(
	"Attempt to deactivate connection while " +
	m_Trans.get()->description() + " still open");

  if (m_reactivation_avoidance.get())
  {
    process_notice("Attempt to deactivate connection while it is in a state "
	"that cannot be fully recovered later (ignoring)");
    return;
  }

  m_Completed = false;
  m_Conn = m_policy.do_disconnect(m_Conn);
}


void pqxx::connection_base::simulate_failure()
{
  if (m_Conn)
  {
    m_Conn = m_policy.do_disconnect(m_Conn);
    inhibit_reactivation(true);
  }
}


int pqxx::connection_base::protocol_version() const noexcept
{
  return m_Conn ? PQprotocolVersion(m_Conn) : 0;
}


int pqxx::connection_base::server_version() const noexcept
{
  return m_serverversion;
}


void pqxx::connection_base::set_variable(const std::string &Var,
	const std::string &Value)
{
  if (m_Trans.get())
  {
    // We're in a transaction.  The variable should go in there.
    m_Trans.get()->set_variable(Var, Value);
  }
  else
  {
    // We're not in a transaction.  Set a session variable.
    if (is_open()) RawSetVar(Var, Value);
    m_Vars[Var] = Value;
  }
}


std::string pqxx::connection_base::get_variable(const std::string &Var)
{
  return m_Trans.get() ? m_Trans.get()->get_variable(Var) : RawGetVar(Var);
}


std::string pqxx::connection_base::RawGetVar(const std::string &Var)
{
  // Is this variable in our local map of set variables?
  // TODO: Could we safely read-allocate variables into m_Vars?
  const auto i = m_Vars.find(Var);
  if (i != m_Vars.end()) return i->second;

  return Exec(("SHOW " + Var).c_str(), 0).at(0).at(0).as(std::string());
}


void pqxx::connection_base::clearcaps() noexcept
{
  m_caps.reset();
}


/** Set up various parts of logical connection state that may need to be
 * recovered because the physical connection to the database was lost and is
 * being reset, or that may not have been initialized yet.
 */
void pqxx::connection_base::SetupState()
{
  if (!m_Conn)
    throw internal_error("SetupState() on no connection");

  if (Status() != CONNECTION_OK)
  {
    const auto Msg = ErrMsg();
    m_Conn = m_policy.do_disconnect(m_Conn);
    throw failure(Msg);
  }

  read_capabilities();

  for (auto &p: m_prepared) p.second.registered = false;

  PQsetNoticeProcessor(m_Conn, pqxx_notice_processor, this);

  InternalSetTrace();

  if (!m_receivers.empty() || !m_Vars.empty())
  {
    std::stringstream restore_query;

    // Pipeline all queries needed to restore receivers and variables, so we can
    // send them over in one go.

    // Reinstate all active receivers
    if (!m_receivers.empty())
    {
      std::string Last;
      for (auto &i: m_receivers)
      {
        // m_receivers can handle multiple receivers waiting on the same event;
        // issue just one LISTEN for each event.
        if (i.first != Last)
        {
          restore_query << "LISTEN \"" << i.first << "\"; ";
          Last = i.first;
        }
      }
    }

    for (auto &i: m_Vars)
      restore_query << "SET " << i.first << "=" << i.second << "; ";

    // Now do the whole batch at once
    PQsendQuery(m_Conn, restore_query.str().c_str());
    result r;
    do
      r = make_result(PQgetResult(m_Conn), "[RECONNECT]");
    while (gate::result_connection(r));
  }

  m_Completed = true;
  if (!is_open()) throw broken_connection();
}


void pqxx::connection_base::check_result(const result &R)
{
  if (!is_open()) throw broken_connection();

  // A shame we can't quite detect out-of-memory to turn this into a bad_alloc!
  if (!gate::result_connection(R)) throw failure(ErrMsg());

  gate::result_creation(R).CheckStatus();
}


void pqxx::connection_base::disconnect() noexcept
{
  // When we activate again, the server may be different!
  clearcaps();

  m_Conn = m_policy.do_disconnect(m_Conn);
}


bool pqxx::connection_base::is_open() const noexcept
{
  return m_Conn && m_Completed && (Status() == CONNECTION_OK);
}


void pqxx::connection_base::process_notice_raw(const char msg[]) noexcept
{
  if (!msg || !*msg) return;
  const auto
	rbegin = m_errorhandlers.rbegin(),
	rend = m_errorhandlers.rend();
  for (auto i = rbegin; i != rend && (**i)(msg); ++i) ;
}


void pqxx::connection_base::process_notice(const char msg[]) noexcept
{
  if (!msg) return;
  const auto len = strlen(msg);
  if (len == 0) return;
  if (msg[len-1] == '\n')
  {
    process_notice_raw(msg);
  }
  else try
  {
    // Newline is missing.  Try the C++ string version of this function.
    process_notice(std::string(msg));
  }
  catch (const std::exception &)
  {
    // If we can't even do that, use plain old buffer copying instead
    // (unavoidably, this will break up overly long messages!)
    const char separator[] = "[...]\n";
    char buf[1007];
    size_t bytes = sizeof(buf)-sizeof(separator)-1;
    size_t written;
    strcpy(&buf[bytes], separator);
    // Write all chunks but last.  Each will fill the buffer exactly.
    for (written = 0; (written+bytes) < len; written += bytes)
    {
      memcpy(buf, &msg[written], bytes);
      process_notice_raw(buf);
    }
    // Write any remaining bytes (which won't fill an entire buffer)
    bytes = len-written;
    memcpy(buf, &msg[written], bytes);
    // Add trailing nul byte, plus newline unless there already is one
    strcpy(&buf[bytes], &"\n"[buf[bytes-1]=='\n']);
    process_notice_raw(buf);
  }
}

void pqxx::connection_base::process_notice(const std::string &msg) noexcept
{
  // Ensure that message passed to errorhandler ends in newline
  if (msg[msg.size()-1] == '\n')
  {
    process_notice_raw(msg.c_str());
  }
  else try
  {
    const std::string nl = msg + "\n";
    process_notice_raw(nl.c_str());
  }
  catch (const std::exception &)
  {
    // If nothing else works, try writing the message without the newline
    process_notice_raw(msg.c_str());
    // This is ugly.
    process_notice_raw("\n");
  }
}


void pqxx::connection_base::trace(FILE *Out) noexcept
{
  m_Trace = Out;
  if (m_Conn) InternalSetTrace();
}


void pqxx::connection_base::add_receiver(pqxx::notification_receiver *T)
{
  if (!T) throw argument_error("Null receiver registered");

  // Add to receiver list and attempt to start listening.
  const auto p = m_receivers.find(T->channel());
  const receiver_list::value_type NewVal(T->channel(), T);

  if (p == m_receivers.end())
  {
    // Not listening on this event yet, start doing so.
    const std::string LQ("LISTEN \"" + T->channel() + "\"");

    if (is_open()) try
    {
      check_result(make_result(PQexec(m_Conn, LQ.c_str()), LQ));
    }
    catch (const broken_connection &)
    {
    }
    m_receivers.insert(NewVal);
  }
  else
  {
    m_receivers.insert(p, NewVal);
  }
}


void pqxx::connection_base::remove_receiver(pqxx::notification_receiver *T)
	noexcept
{
  if (!T) return;

  try
  {
    // Keep Sun compiler happy...  Hope it doesn't annoy other compilers
    std::pair<const std::string, notification_receiver *> tmp_pair(
	T->channel(), T);
    auto E = tmp_pair;
    auto R = m_receivers.equal_range(E.first);
    const auto i = find(R.first, R.second, E);

    if (i == R.second)
    {
      process_notice("Attempt to remove unknown receiver '" + E.first + "'");
    }
    else
    {
      // Erase first; otherwise a notification for the same receiver may yet
      // come in and wreak havoc.  Thanks Dragan Milenkovic.
      const bool gone = (m_Conn && (R.second == ++R.first));
      m_receivers.erase(i);
      if (gone) Exec(("UNLISTEN \"" + T->channel() + "\"").c_str(), 0);
    }
  }
  catch (const std::exception &e)
  {
    process_notice(e.what());
  }
}


bool pqxx::connection_base::consume_input() noexcept
{
  return PQconsumeInput(m_Conn) != 0;
}


bool pqxx::connection_base::is_busy() const noexcept
{
  return PQisBusy(m_Conn) != 0;
}


namespace
{
/// Stateful libpq "cancel" operation.
class cancel_wrapper
{
  PGcancel *m_cancel;
  char m_errbuf[500];

public:
  cancel_wrapper(PGconn *conn) :
    m_cancel(nullptr),
    m_errbuf()
  {
    if (conn)
    {
      m_cancel = PQgetCancel(conn);
      if (!m_cancel) throw std::bad_alloc();
    }
  }
  ~cancel_wrapper() { if (m_cancel) PQfreeCancel(m_cancel); }

  void operator()()
  {
    if (m_cancel && !PQcancel(m_cancel, m_errbuf, int(sizeof(m_errbuf))))
      throw sql_error(std::string(m_errbuf));
  }
};
}


void pqxx::connection_base::cancel_query()
{
  cancel_wrapper cancel(m_Conn);
  cancel();
}

void pqxx::connection_base::set_verbosity(error_verbosity verbosity) noexcept
{
    PQsetErrorVerbosity(m_Conn, static_cast<PGVerbosity>(verbosity));
    m_verbosity = verbosity;
}


namespace
{
/// Unique pointer to PGnotify.
typedef std::unique_ptr<PGnotify, void (*)(PGnotify *)> notify_ptr;


/// Get one notification from a connection, or null.
notify_ptr get_notif(pqxx::internal::pq::PGconn *conn)
{
  return notify_ptr(PQnotifies(conn), freepqmem_templated<PGnotify>);
}
}


int pqxx::connection_base::get_notifs()
{
  if (!is_open()) return 0;

  if (!consume_input()) throw broken_connection();

  // Even if somehow we receive notifications during our transaction, don't
  // deliver them.
  if (m_Trans.get()) return 0;

  int notifs = 0;
  for (auto N = get_notif(m_Conn); N.get(); N = get_notif(m_Conn))
  {
    notifs++;

    const auto Hit = m_receivers.equal_range(std::string(N->relname));
    for (auto i = Hit.first; i != Hit.second; ++i) try
    {
      (*i->second)(N->extra, N->be_pid);
    }
    catch (const std::exception &e)
    {
      try
      {
        process_notice(
		"Exception in notification receiver '" +
		i->first +
		"': " +
		e.what() +
		"\n");
      }
      catch (const std::bad_alloc &)
      {
        // Out of memory.  Try to get the message out in a more robust way.
        process_notice(
		"Exception in notification receiver, "
		"and also ran out of memory\n");
      }
      catch (const std::exception &)
      {
        process_notice(
		"Exception in notification receiver "
		"(compounded by other error)\n");
      }
    }

    N.reset();
  }
  return notifs;
}


const char *pqxx::connection_base::dbname()
{
  if (!m_Conn) activate();
  return PQdb(m_Conn);
}


const char *pqxx::connection_base::username()
{
  if (!m_Conn) activate();
  return PQuser(m_Conn);
}


const char *pqxx::connection_base::hostname()
{
  if (!m_Conn) activate();
  return PQhost(m_Conn);
}


const char *pqxx::connection_base::port()
{
  if (!m_Conn) activate();
  return PQport(m_Conn);
}


const char *pqxx::connection_base::ErrMsg() const noexcept
{
  return m_Conn ? PQerrorMessage(m_Conn) : "No connection to database";
}


void pqxx::connection_base::register_errorhandler(errorhandler *handler)
{
  m_errorhandlers.push_back(handler);
}


void pqxx::connection_base::unregister_errorhandler(errorhandler *handler)
  noexcept
{
  // The errorhandler itself will take care of nulling its pointer to this
  // connection.
  m_errorhandlers.remove(handler);
}


std::vector<errorhandler *> pqxx::connection_base::get_errorhandlers() const
{
  std::vector<errorhandler *> handlers(m_errorhandlers.size());
  std::copy(m_errorhandlers.begin(), m_errorhandlers.end(), handlers.begin());
  return handlers;
}


pqxx::result pqxx::connection_base::Exec(const char Query[], int Retries)
{
  activate();

  auto R = make_result(PQexec(m_Conn, Query), Query);

  while ((Retries > 0) && !gate::result_connection(R) && !is_open())
  {
    Retries--;
    Reset();
    if (is_open()) R = make_result(PQexec(m_Conn, Query), Query);
  }

  check_result(R);

  get_notifs();
  return R;
}


void pqxx::connection_base::prepare(
	const std::string &name,
	const std::string &definition)
{
  auto i = m_prepared.find(name);
  if (i != m_prepared.end())
  {
    if (definition != i->second.definition)
    {
      if (!name.empty())
        throw argument_error(
		"Inconsistent redefinition of prepared statement " + name);

      i->second.registered = false;
      i->second.definition = definition;
    }
  }
  else
  {
    m_prepared.insert(make_pair(
	name,
	prepare::internal::prepared_def(definition)));
  }
}


void pqxx::connection_base::prepare(const std::string &definition)
{
  this->prepare(std::string(), definition);
}


void pqxx::connection_base::unprepare(const std::string &name)
{
  auto i = m_prepared.find(name);

  // Quietly ignore duplicated or spurious unprepare()s
  if (i == m_prepared.end()) return;

  if (i->second.registered) Exec(("DEALLOCATE \"" + name + "\"").c_str(), 0);

  m_prepared.erase(i);
}


pqxx::prepare::internal::prepared_def &
pqxx::connection_base::find_prepared(const std::string &statement)
{
  auto s = m_prepared.find(statement);
  if (s == m_prepared.end())
    throw argument_error("Unknown prepared statement '" + statement + "'");
  return s->second;
}


pqxx::prepare::internal::prepared_def &
pqxx::connection_base::register_prepared(const std::string &name)
{
  activate();
  auto &s = find_prepared(name);

  // "Register" (i.e., define) prepared statement with backend on demand
  if (!s.registered)
  {
    auto r = make_result(
      PQprepare(m_Conn, name.c_str(), s.definition.c_str(), 0, nullptr),
      "[PREPARE " + name + "]");
    check_result(r);
    s.registered = !name.empty();
    return s;
  }

  return s;
}

void pqxx::connection_base::prepare_now(const std::string &name)
{
  register_prepared(name);
}


// TODO: Can we make this work with std::string instead of C-style?
pqxx::result pqxx::connection_base::prepared_exec(
	const std::string &statement,
	const char *const params[],
	const int paramlengths[],
	const int binary[],
	int nparams)
{
  register_prepared(statement);
  activate();
  auto r = make_result(
	PQexecPrepared(
		m_Conn,
		statement.c_str(),
		nparams,
		params,
		paramlengths,
		binary,
		0),
    	statement);
  check_result(r);
  get_notifs();
  return r;
}


bool pqxx::connection_base::prepared_exists(const std::string &statement) const
{
  auto s = m_prepared.find(statement);
  return s != PSMap::const_iterator(m_prepared.end());
}


void pqxx::connection_base::Reset()
{
  if (m_inhibit_reactivation)
    throw broken_connection(
	"Could not reset connection: reactivation is inhibited");
  if (m_reactivation_avoidance.get()) return;

  // TODO: Probably need to go through a full disconnect/reconnect!
  // Forget about any previously ongoing connection attempts
  m_Conn = m_policy.do_dropconnect(m_Conn);
  m_Completed = false;

  if (m_Conn)
  {
    // Reset existing connection
    PQreset(m_Conn);
    SetupState();
  }
  else
  {
    // No existing connection--start a new one
    activate();
  }
}


void pqxx::connection_base::close() noexcept
{
  m_Completed = false;
  inhibit_reactivation(false);
  m_reactivation_avoidance.clear();
  try
  {
    if (m_Trans.get())
      process_notice("Closing connection while " +
	             m_Trans.get()->description() + " still open");

    if (!m_receivers.empty())
    {
      process_notice("Closing connection with outstanding receivers.");
      m_receivers.clear();
    }

    PQsetNoticeProcessor(m_Conn, nullptr, nullptr);
    std::list<errorhandler *> old_handlers;
    m_errorhandlers.swap(old_handlers);
    const auto
	rbegin = old_handlers.rbegin(),
	rend = old_handlers.rend();
    for (auto i = rbegin; i!=rend; ++i)
      gate::errorhandler_connection_base(**i).unregister();

    m_Conn = m_policy.do_disconnect(m_Conn);
  }
  catch (...)
  {
  }
}


void pqxx::connection_base::RawSetVar(const std::string &Var,
	const std::string &Value)
{
    Exec(("SET " + Var + "=" + Value).c_str(), 0);
}


void pqxx::connection_base::AddVariables(
	const std::map<std::string,std::string> &Vars)
{
  for (auto &i: Vars) m_Vars[i.first] = i.second;
}


void pqxx::connection_base::InternalSetTrace() noexcept
{
  if (m_Conn)
  {
    if (m_Trace) PQtrace(m_Conn, m_Trace);
    else PQuntrace(m_Conn);
  }
}


int pqxx::connection_base::Status() const noexcept
{
  return PQstatus(m_Conn);
}


void pqxx::connection_base::RegisterTransaction(transaction_base *T)
{
  m_Trans.Register(T);
}


void pqxx::connection_base::UnregisterTransaction(transaction_base *T) noexcept
{
  try
  {
    m_Trans.Unregister(T);
  }
  catch (const std::exception &e)
  {
    process_notice(e.what());
  }
}


bool pqxx::connection_base::ReadCopyLine(std::string &Line)
{
  if (!is_open())
    throw internal_error("ReadCopyLine() without connection");

  Line.erase();
  bool Result;

  char *Buf = 0;
  const std::string query = "[END COPY]";
  switch (PQgetCopyData(m_Conn, &Buf, false))
  {
    case -2:
      throw failure("Reading of table data failed: " + std::string(ErrMsg()));

    case -1:
      for (auto R = make_result(PQgetResult(m_Conn), query);
           gate::result_connection(R);
	   R=make_result(PQgetResult(m_Conn), query))
	check_result(R);
      Result = false;
      break;

    case 0:
      throw internal_error("table read inexplicably went asynchronous");

    default:
      if (Buf)
      {
        std::unique_ptr<char, void (*)(char *)> PQA(
            Buf, freepqmem_templated<char>);
        Line = Buf;
      }
      Result = true;
  }

  return Result;
}


void pqxx::connection_base::WriteCopyLine(const std::string &Line)
{
  if (!is_open())
    throw internal_error("WriteCopyLine() without connection");

  const std::string L = Line + '\n';
  const char *const LC = L.c_str();
  const auto Len = L.size();

  if (PQputCopyData(m_Conn, LC, int(Len)) <= 0)
  {
    const std::string Msg = std::string("Error writing to table: ") + ErrMsg();
    PQendcopy(m_Conn);
    throw failure(Msg);
  }
}


void pqxx::connection_base::EndCopyWrite()
{
  int Res = PQputCopyEnd(m_Conn, nullptr);
  switch (Res)
  {
  case -1:
    throw failure("Write to table failed: " + std::string(ErrMsg()));
  case 0:
    throw internal_error("table write is inexplicably asynchronous");
  case 1:
    // Normal termination.  Retrieve result object.
    break;

  default:
    throw internal_error(
	"unexpected result " + to_string(Res) + " from PQputCopyEnd()");
  }

  check_result(make_result(PQgetResult(m_Conn), "[END COPY]"));
}


void pqxx::connection_base::start_exec(const std::string &Q)
{
  activate();
  if (!PQsendQuery(m_Conn, Q.c_str())) throw failure(ErrMsg());
}


pqxx::internal::pq::PGresult *pqxx::connection_base::get_result()
{
  if (!m_Conn) throw broken_connection();
  return PQgetResult(m_Conn);
}


void pqxx::connection_base::add_reactivation_avoidance_count(int n)
{
  m_reactivation_avoidance.add(n);
}


std::string pqxx::connection_base::esc(const char str[], size_t maxlen)
{
  std::string escaped;

  // We need a connection object...  This is the one reason why this function is
  // not const!
  if (!m_Conn) activate();

  char *const buf = new char[2*maxlen+1];
  try
  {
    int err = 0;
    PQescapeStringConn(m_Conn, buf, str, maxlen, &err);
    if (err) throw argument_error(ErrMsg());
    escaped = std::string(buf);
  }
  catch (const std::exception &)
  {
    delete [] buf;
    throw;
  }
  delete [] buf;

  return escaped;
}


std::string pqxx::connection_base::esc(const char str[])
{
  return this->esc(str, strlen(str));
}


std::string pqxx::connection_base::esc(const std::string &str)
{
  return this->esc(str.c_str(), str.size());
}


std::string pqxx::connection_base::esc_raw(
        const unsigned char str[],
        size_t len)
{
  size_t bytes = 0;
  // We need a connection object...  This is the one reason why this function is
  // not const!
  activate();

  std::unique_ptr<unsigned char, void (*)(unsigned char *)> buf(
	PQescapeByteaConn(m_Conn, str, len, &bytes),
	freepqmem_templated<unsigned char>);
  if (!buf.get()) throw std::bad_alloc();
  return std::string(reinterpret_cast<char *>(buf.get()));
}


std::string pqxx::connection_base::unesc_raw(const char *text)
{
  size_t len;
  unsigned char *bytes = const_cast<unsigned char *>(
	reinterpret_cast<const unsigned char *>(text));
  const unsigned char *const buf = PQunescapeBytea(bytes, &len);
  return std::string(buf, buf + len);
}


std::string pqxx::connection_base::quote_raw(
        const unsigned char str[],
        size_t len)
{
  return "'" + esc_raw(str, len) + "'::bytea";
}


std::string pqxx::connection_base::quote(const binarystring &b)
{
  return quote_raw(b.data(), b.size());
}


std::string pqxx::connection_base::quote_name(const std::string &identifier)
{
  // We need a connection object...  This is the one reason why this function is
  // not const!
  activate();
  std::unique_ptr<char, void (*)(char *)> buf(
	PQescapeIdentifier(m_Conn, identifier.c_str(), identifier.size()),
        freepqmem_templated<char>);
  if (!buf.get()) throw failure(ErrMsg());
  return std::string(buf.get());
}


pqxx::internal::reactivation_avoidance_exemption::
  reactivation_avoidance_exemption(
	connection_base &C) :
  m_home(C),
  m_count(gate::connection_reactivation_avoidance_exemption(C).get_counter()),
  m_open(C.is_open())
{
  gate::connection_reactivation_avoidance_exemption gate(C);
  gate.clear_counter();
}


pqxx::internal::reactivation_avoidance_exemption::
  ~reactivation_avoidance_exemption()
{
  // Don't leave the connection open if reactivation avoidance is in effect and
  // the connection needed to be reactivated temporarily.
  if (m_count && !m_open) m_home.deactivate();
  gate::connection_reactivation_avoidance_exemption gate(m_home);
  gate.add_counter(m_count);
}


namespace
{
void wait_fd(int fd, bool forwrite=false, timeval *tv=0)
{
  if (fd < 0) throw pqxx::broken_connection();

#ifdef PQXX_HAVE_POLL
  pollfd pfd = {
    fd,
    short(POLLERR|POLLHUP|POLLNVAL | (forwrite?POLLOUT:POLLIN)),
    0,
    };
  poll(&pfd, 1, (tv ? int(tv->tv_sec*1000 + tv->tv_usec/1000) : -1));
#else
  fd_set s;
  clear_fdmask(&s);
  set_fdbit(fd, &s);
  select(fd+1, (forwrite?fdset_none:&s), (forwrite?&s:fdset_none), &s, tv);
#endif
}
} // namespace

void pqxx::internal::wait_read(const pq::PGconn *c)
{
  wait_fd(socket_of(c));
}


void pqxx::internal::wait_read(const pq::PGconn *c,
    long seconds,
    long microseconds)
{
  // These are really supposed to be time_t and suseconds_t.  But not all
  // platforms have that type; some use "long" instead, and some 64-bit
  // systems use 32-bit integers here.  So "int" seems to be the only really
  // safe type to use.
  timeval tv = { time_t(seconds), int(microseconds) };
  wait_fd(socket_of(c), false, &tv);
}


void pqxx::internal::wait_write(const pq::PGconn *c)
{
  wait_fd(socket_of(c), true);
}


void pqxx::connection_base::wait_read() const
{
  internal::wait_read(m_Conn);
}


void pqxx::connection_base::wait_read(long seconds, long microseconds) const
{
  internal::wait_read(m_Conn, seconds, microseconds);
}


void pqxx::connection_base::wait_write() const
{
  internal::wait_write(m_Conn);
}


int pqxx::connection_base::await_notification()
{
  activate();
  int notifs = get_notifs();
  if (!notifs)
  {
    wait_read();
    notifs = get_notifs();
  }
  return notifs;
}


int pqxx::connection_base::await_notification(long seconds, long microseconds)
{
  activate();
  int notifs = get_notifs();
  if (!notifs)
  {
    wait_read(seconds, microseconds);
    notifs = get_notifs();
  }
  return notifs;
}


void pqxx::connection_base::read_capabilities()
{
  m_serverversion = PQserverVersion(m_Conn);
  if (m_serverversion <= 90000)
    throw feature_not_supported(
	"Unsupported server version; 9.0 is the minimum.");

  switch (protocol_version()) {
  case 0:
    throw broken_connection();
  case 1:
  case 2:
    throw feature_not_supported(
        "Unsupported frontend/backend protocol version; 3.0 is the minimum.");
  default:
    break;
  }

  m_caps[cap_prepared_statements] = true;
  m_caps[cap_statement_varargs] = true;
  m_caps[cap_prepare_unnamed_statement] = true;
  m_caps[cap_cursor_scroll] = true;
  m_caps[cap_cursor_with_hold] = true;
  m_caps[cap_cursor_fetch_0] = true;
  m_caps[cap_nested_transactions] = true;
  m_caps[cap_create_table_with_oids] = true;
  m_caps[cap_read_only_transactions] = true;
  m_caps[cap_notify_payload] = true;
  m_caps[cap_table_column] = true;
  m_caps[cap_parameterized_statements] = true;
}


std::string pqxx::connection_base::adorn_name(const std::string &n)
{
  const std::string id = to_string(++m_unique_id);
  return n.empty() ? ("x"+id) : (n+"_"+id);
}


int pqxx::connection_base::encoding_code()
{
  activate();
  return PQclientEncoding(m_Conn);
}


pqxx::result pqxx::connection_base::parameterized_exec(
	const std::string &query,
	const char *const params[],
	const int paramlengths[],
	const int binaries[],
	int nparams)
{
  auto r = make_result(
  	PQexecParams(
		m_Conn,
		query.c_str(),
		nparams,
		nullptr,
		params,
		paramlengths,
		binaries,
		0),
	query);
  check_result(r);
  get_notifs();
  return r;
}
