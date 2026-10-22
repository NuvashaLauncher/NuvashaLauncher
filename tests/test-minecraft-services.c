/* Tests for the centralized Minecraft Services request policy. */
#include "nuvasha-minecraft-services.h"

#include <glib.h>

static void
test_message (void)
{
  g_autoptr (SoupMessage) message =
    nuvasha_minecraft_services_message (SOUP_METHOD_GET, "/minecraft/profile",
                                        "token");
  SoupMessageHeaders *headers = soup_message_get_request_headers (message);

  g_assert_nonnull (message);
  g_assert_cmpstr (g_uri_get_path (soup_message_get_uri (message)), ==,
                   "/minecraft/profile");
  g_assert_cmpstr (soup_message_headers_get_one (headers, "Accept"), ==,
                   "application/json");
  g_assert_cmpstr (soup_message_headers_get_one (headers, "Authorization"), ==,
                   "Bearer token");
}

static void
test_retry_policy (void)
{
  g_autoptr (SoupMessage) message = soup_message_new (SOUP_METHOD_GET,
                                                       "https://example.invalid");
  SoupMessageHeaders *headers = soup_message_get_response_headers (message);

  g_assert_cmpuint (nuvasha_minecraft_services_retry_delay (message, 0), ==, 1);
  soup_message_headers_replace (headers, "Retry-After", "23");
  g_assert_cmpuint (nuvasha_minecraft_services_retry_delay (message, 0), ==, 23);
  soup_message_headers_remove (headers, "Retry-After");
  g_assert_cmpuint (nuvasha_minecraft_services_retry_delay (message, 8), ==, 120);
}

static void
test_error_domain (void)
{
  g_autoptr (GError) error =
    nuvasha_minecraft_services_response_error (NULL, NULL);

  g_assert_error (error, nuvasha_MINECRAFT_SERVICES_ERROR,
                  nuvasha_MINECRAFT_SERVICES_ERROR_HTTP);
}

static void
test_skin_multipart (void)
{
  g_autoptr (GBytes) skin = g_bytes_new_static ("PNG", 3);
  g_autofree char *content_type = NULL;
  g_autoptr (GBytes) body =
    nuvasha_minecraft_services_skin_multipart (skin, "SLIM", &content_type);
  gsize length = 0;
  const char *data = g_bytes_get_data (body, &length);

  g_assert_true (g_str_has_prefix (content_type, "multipart/form-data; boundary="));
  g_assert_cmpint (length, >, 3);
  g_assert_nonnull (g_strstr_len (data, length, "name=\"file\""));
  g_assert_nonnull (g_strstr_len (data, length, "name=\"variant\""));
  g_assert_nonnull (g_strstr_len (data, length, "SLIM"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/minecraft-services/message", test_message);
  g_test_add_func ("/minecraft-services/retry-policy", test_retry_policy);
  g_test_add_func ("/minecraft-services/error-domain", test_error_domain);
  g_test_add_func ("/minecraft-services/skin-multipart", test_skin_multipart);
  return g_test_run ();
}
