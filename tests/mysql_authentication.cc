#include <thread>
#include <iostream>

#include <silicon/api.hh>
#include <silicon/middleware_factories.hh>
#include <silicon/backends/mhd.hh>
#include <silicon/middlewares/mysql_connection.hh>
#include <silicon/middlewares/mysql_session.hh>
#include <silicon/middlewares/mysql_orm.hh>
#include <silicon/clients/libcurl_client.hh>
#include "symbols.hh"

using namespace s;

using namespace sl;

// User database type
typedef decltype(D(_id(_auto_increment, _primary_key) = int(),
                       _name = std::string()
                   )) User;

// ==================================================
// Session
//   Simply store the user_id
struct session_data
{
  session_data() { user_id = -1; }
  bool authenticated() const { return user_id != -1; }
  static auto sio_info() { return D(_user_id = int()); }
  int user_id;
};

// Wrap the session data in a mysql session middleware.
typedef mysql_session<session_data> session;

// ==================================================
// Current user middleware.
//  All procedures taking current_user as argument will
//  throw error::unauthorized if the user is not authenticated.
struct current_user : public User
{
  User& user_data() { return *static_cast<User*>(this); }

  // Requires the session and a mysql connection.
  static auto instantiate(session& sess, mysql_orm<User> orm)
  {
    if (!sess->authenticated())
      throw error::unauthorized("Access to this procedure is reserved to logged users.");

    current_user u;

    std::cout << sess->user_id << std::endl;
    if (!orm.find_by_id(sess->user_id, u.user_data()))
      throw error::internal_server_error("Session user_id not in user table.");

    return u;
  }
};

// Authenticator middleware
struct authenticator
{
  authenticator(session& _sess, mysql_connection& _con) : sess(_sess), con(_con) {}

  bool authenticate(std::string name)
  {
    int count = 0;
    int user_id;
    // 1/ check if the user is valid.
    if (con("SELECT id from users where name = ?")(name) >> user_id)
    {
      // 2/ store data in the session.
      sess->user_id = user_id;
      sess.save();
      return true;
    }
    else
    return false;
  };

  // Requires the session and a mysql connection.
  static auto instantiate(session& sess, mysql_connection& con)
  {
    return authenticator(sess, con);
  }

  session sess;
  mysql_connection con;
};

int main()
{
  using namespace sl;

  auto api = http_api(
    GET / _who_am_I = [] (current_user& user)
    {
      return user.user_data();
    },

    POST / _signin * post_parameters(_name = std::string()) = [] (auto params, authenticator& auth)
    {
      if (!auth.authenticate(params.name))
        throw error::bad_request("Invalid user name");
    },

    GET / _logout = [] (session& sess)
    {
      sess.destroy();
    }

    );

  auto mf = middleware_factories(
      mysql_connection_factory("localhost", "silicon", "my_silicon", "silicon_test"), // mysql middleware.
      mysql_orm_factory<User>("users"), // Orm middleware for users.
      mysql_session_factory<session_data>("sessions"),
      mhd_session_cookie()
      );

  
  { // Setup database for testing.

    // Drop user table.
    auto con = instantiate_factory<mysql_connection>(mf);
    con("DROP table if exists users;")();
    con("DROP table if exists session;")();

    // Create user table.
    mysql_orm_factory<User>("users").initialize(con);

    // Insert data.
    auto orm = instantiate_factory<mysql_orm<User>>(mf);
    std::cout << orm.insert(User(0, "John Doe")) << std::endl;
  }
  
  // Start server.
  auto server = mhd_json_serve(api, mf, 12345, _non_blocking);

  // Test.
  auto c = libcurl_json_client(api, "127.0.0.1", 12345);

  // Signin.
  auto signin_r = c.http_post.signin(_name = "John Doe");
  std::cout << json_encode(signin_r) << std::endl;
  
  assert(signin_r.status == 200);

  auto who_r = c.http_get.who_am_I();
  std::cout << json_encode(who_r) << std::endl;
  assert(who_r.status == 200);
  assert(who_r.response.name == "John Doe");

  auto logout_r = c.http_get.logout();
  std::cout << json_encode(logout_r) << std::endl;
  assert(logout_r.status == 200);

  auto who_r2 = c.http_get.who_am_I();
  std::cout << who_r2.status << " " << who_r2.error << std::endl;
  assert(who_r2.status == 401);
  
}
