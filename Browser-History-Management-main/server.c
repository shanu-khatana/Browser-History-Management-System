#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <time.h>
#include "sqlite3.h"

#pragma comment(lib, "ws2_32.lib")

#define PORT 8080
#define BUFFER_SIZE 16384
#define MAX_URL_LENGTH 2048
#define DB_PATH "history.db"

WSADATA wsa;
sqlite3 *db = NULL;

// -------------------- Utilities --------------------
void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) &&
            (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
            int hi, lo;
            if (a >= 'a') hi = a - 'a' + 10;
            else if (a >= 'A') hi = a - 'A' + 10;
            else hi = a - '0';
            if (b >= 'a') lo = b - 'a' + 10;
            else if (b >= 'A') lo = b - 'A' + 10;
            else lo = b - '0';
            *dst++ = (char)(hi * 16 + lo);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

void send_response(SOCKET client_socket, const char *status, const char *content_type, const char *body) {
    char response[BUFFER_SIZE * 2];
    int body_len = (int)strlen(body);
    int n = snprintf(response, sizeof(response),
            "HTTP/1.1 %s\r\n"
            "Content-Type: %s\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS, DELETE\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            status, content_type, body_len, body);
    if (n > 0) send(client_socket, response, n, 0);
}

// -------------------- Database Init --------------------
int init_db() {
    int rc = sqlite3_open(DB_PATH, &db);
    if (rc != SQLITE_OK) {
        printf("Cannot open database: %s\n", sqlite3_errmsg(db));
        return rc;
    }

    const char *create_visits =
        "CREATE TABLE IF NOT EXISTS visits ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "username TEXT NOT NULL,"
        "url TEXT NOT NULL,"
        "visits INTEGER NOT NULL DEFAULT 1,"
        "pos INTEGER NOT NULL,"
        "is_current INTEGER NOT NULL DEFAULT 0,"
        "UNIQUE(username, url)"
        ");";

    const char *create_bookmarks =
        "CREATE TABLE IF NOT EXISTS bookmarks ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "username TEXT NOT NULL,"
        "url TEXT NOT NULL,"
        "added_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "UNIQUE(username, url)"
        ");";

    char *err = NULL;
    rc = sqlite3_exec(db, create_visits, 0, 0, &err);
    if (rc != SQLITE_OK) {
        printf("SQL error (visits): %s\n", err);
        sqlite3_free(err);
        return rc;
    }
    rc = sqlite3_exec(db, create_bookmarks, 0, 0, &err);
    if (rc != SQLITE_OK) {
        printf("SQL error (bookmarks): %s\n", err);
        sqlite3_free(err);
        return rc;
    }

    return SQLITE_OK;
}

// -------------------- DB helpers --------------------
long get_max_pos_for_user(const char *username) {
    const char *sql = "SELECT IFNULL(MAX(pos), 0) FROM visits WHERE username = ?;";
    sqlite3_stmt *stmt = NULL;
    long maxpos = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            maxpos = sqlite3_column_int64(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    return maxpos;
}

void unset_current_for_user(const char *username) {
    const char *sql = "UPDATE visits SET is_current = 0 WHERE username = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

void add_visit_to_db(const char *username, const char *url) {
    if (!username || !url) return;
    const char *find_sql = "SELECT id, visits FROM visits WHERE username = ? AND url = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, find_sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, url, -1, SQLITE_STATIC);

    long maxpos = get_max_pos_for_user(username);
    long newpos = maxpos + 1;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        int visits = sqlite3_column_int(stmt, 1) + 1;
        sqlite3_finalize(stmt);

        unset_current_for_user(username);
        const char *upd_sql = "UPDATE visits SET visits = ?, pos = ?, is_current = 1 WHERE id = ?;";
        sqlite3_stmt *upd = NULL;
        if (sqlite3_prepare_v2(db, upd_sql, -1, &upd, NULL) == SQLITE_OK) {
            sqlite3_bind_int(upd, 1, visits);
            sqlite3_bind_int64(upd, 2, newpos);
            sqlite3_bind_int(upd, 3, id);
            sqlite3_step(upd);
        }
        sqlite3_finalize(upd);
    } else {
        sqlite3_finalize(stmt);
        unset_current_for_user(username);
        const char *ins_sql = "INSERT OR IGNORE INTO visits (username, url, visits, pos, is_current) VALUES (?, ?, 1, ?, 1);";
        sqlite3_stmt *ins = NULL;
        if (sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ins, 1, username, -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 2, url, -1, SQLITE_STATIC);
            sqlite3_bind_int64(ins, 3, newpos);
            sqlite3_step(ins);
        }
        sqlite3_finalize(ins);
    }
}

void get_history_json_for_user(const char *username, char *out_buf, size_t out_buf_size) {
    if (!username || !out_buf) return;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT url, visits, is_current FROM visits WHERE username = ? ORDER BY pos ASC;";
    size_t used = 0;
    used += snprintf(out_buf + used, out_buf_size - used, "{\"history\":[");
    int first = 1;
    int idx = 0;
    int current_pos = -1;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *url = sqlite3_column_text(stmt, 0);
            int visits = sqlite3_column_int(stmt, 1);
            int is_current = sqlite3_column_int(stmt, 2);
            if (is_current) current_pos = idx;

            if (!first) used += snprintf(out_buf + used, out_buf_size - used, ",");
            used += snprintf(out_buf + used, out_buf_size - used,
                             "{\"url\":\"%s\",\"visits\":%d,\"current\":%s}",
                             url ? (const char*)url : "", visits, is_current ? "true" : "false");
            first = 0;
            idx++;
            if (used >= out_buf_size - 256) break; // safety
        }
    }
    sqlite3_finalize(stmt);
    used += snprintf(out_buf + used, out_buf_size - used, "],\"current_position\":%d}", current_pos);
}

void search_history_for_user(const char *username, const char *query, char *out_buf, size_t out_buf_size) {
    if (!username || !out_buf) return;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT url, visits FROM visits WHERE username = ? AND url LIKE ? ORDER BY pos ASC;";
    size_t used = 0;
    used += snprintf(out_buf + used, out_buf_size - used, "{\"results\":[");
    int first = 1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        char likeq[1024];
        if (query && strlen(query) > 0) snprintf(likeq, sizeof(likeq), "%%%s%%", query);
        else snprintf(likeq, sizeof(likeq), "%%");
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, likeq, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *url = sqlite3_column_text(stmt, 0);
            int visits = sqlite3_column_int(stmt, 1);
            if (!first) used += snprintf(out_buf + used, out_buf_size - used, ",");
            used += snprintf(out_buf + used, out_buf_size - used,
                             "{\"url\":\"%s\",\"visits\":%d}", url ? (const char*)url : "", visits);
            first = 0;
            if (used >= out_buf_size - 256) break;
        }
    }
    sqlite3_finalize(stmt);
    used += snprintf(out_buf + used, out_buf_size - used, "]}");
}

int go_back_for_user(const char *username, char *out_url, size_t out_len) {
    if (!username || !out_url) return 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, url, is_current FROM visits WHERE username = ? ORDER BY pos ASC;";
    int prev_id = -1;
    char prev_url[MAX_URL_LENGTH] = {0};
    int found_current = 0;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const unsigned char *url = sqlite3_column_text(stmt, 1);
        int is_current = sqlite3_column_int(stmt, 2);

        if (is_current) {
            found_current = 1;
            break; // prev_id/url holds previous row if any
        }

        prev_id = id;
        if (url) strncpy(prev_url, (const char*)url, sizeof(prev_url)-1);
    }
    sqlite3_finalize(stmt);

    if (found_current && prev_id != -1) {
        unset_current_for_user(username);
        const char *set_sql = "UPDATE visits SET is_current = 1 WHERE id = ?;";
        sqlite3_stmt *s3 = NULL;
        if (sqlite3_prepare_v2(db, set_sql, -1, &s3, NULL) == SQLITE_OK) {
            sqlite3_bind_int(s3, 1, prev_id);
            sqlite3_step(s3);
        }
        sqlite3_finalize(s3);
        strncpy(out_url, prev_url, out_len-1);
        return 1;
    }
    return 0;
}

int go_forward_for_user(const char *username, char *out_url, size_t out_len) {
    if (!username || !out_url) return 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, url, is_current FROM visits WHERE username = ? ORDER BY pos ASC;";
    int seen_current = 0;
    int next_id = -1;
    char next_url[MAX_URL_LENGTH] = {0};

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const unsigned char *url = sqlite3_column_text(stmt, 1);
        int is_current = sqlite3_column_int(stmt, 2);

        if (seen_current) {
            next_id = id;
            if (url) strncpy(next_url, (const char*)url, sizeof(next_url)-1);
            break;
        }
        if (is_current) seen_current = 1;
    }
    sqlite3_finalize(stmt);

    if (next_id != -1) {
        unset_current_for_user(username);
        const char *set_sql = "UPDATE visits SET is_current = 1 WHERE id = ?;";
        sqlite3_stmt *s3 = NULL;
        if (sqlite3_prepare_v2(db, set_sql, -1, &s3, NULL) == SQLITE_OK) {
            sqlite3_bind_int(s3, 1, next_id);
            sqlite3_step(s3);
        }
        sqlite3_finalize(s3);
        strncpy(out_url, next_url, out_len-1);
        return 1;
    }
    return 0;
}

void clear_history_for_user(const char *username) {
    if (!username) return;
    const char *sql = "DELETE FROM visits WHERE username = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

void get_bookmarks_json(const char *username, char *out_buf, size_t out_buf_size) {
    if (!username || !out_buf) return;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT url FROM bookmarks WHERE username = ? ORDER BY added_at DESC;";
    size_t used = 0;
    used += snprintf(out_buf + used, out_buf_size - used, "{\"bookmarks\":[");
    int first = 1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *url = sqlite3_column_text(stmt, 0);
            if (!first) used += snprintf(out_buf + used, out_buf_size - used, ",");
            used += snprintf(out_buf + used, out_buf_size - used, "{\"url\":\"%s\"}", url ? (const char*)url : "");
            first = 0;
            if (used >= out_buf_size - 256) break;
        }
    }
    sqlite3_finalize(stmt);
    used += snprintf(out_buf + used, out_buf_size - used, "]}");
}

void add_bookmark_to_db(const char *username, const char *url) {
    if (!username || !url) return;
    const char *sql = "INSERT OR IGNORE INTO bookmarks (username, url) VALUES (?, ?);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, url, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

void remove_bookmark_from_db(const char *username, const char *url) {
    if (!username || !url) return;
    const char *sql = "DELETE FROM bookmarks WHERE username = ? AND url = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, url, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

// -------------------- Request parsing helpers --------------------
void parse_post_body(const char *body_raw, char *out_url, size_t url_len, char *out_user, size_t user_len) {
    if (!body_raw) {
        if (out_url && url_len) out_url[0] = '\0';
        if (out_user && user_len) out_user[0] = '\0';
        return;
    }

    char tmp[BUFFER_SIZE];
    strncpy(tmp, body_raw, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';

    if (out_url && url_len) out_url[0] = '\0';
    if (out_user && user_len) out_user[0] = '\0';

    char *pair = strtok(tmp, "&");
    while (pair) {
        if (strncmp(pair, "url=", 4) == 0 && out_url && url_len) {
            url_decode(out_url, pair + 4);
        } else if (strncmp(pair, "user=", 5) == 0 && out_user && user_len) {
            url_decode(out_user, pair + 5);
        }
        pair = strtok(NULL, "&");
    }
}

// -------------------- HTTP request handling --------------------
void handle_request(SOCKET client_socket, const char *request) {
    char method[16] = {0}, path[1024] = {0}, body[BUFFER_SIZE] = {0};
    sscanf(request, "%15s %1023s", method, path);

    if (strcmp(method, "OPTIONS") == 0) {
        send_response(client_socket, "200 OK", "text/plain", "");
        return;
    }

    char *body_start = strstr(request, "\r\n\r\n");
    if (body_start) {
        strncpy(body, body_start + 4, sizeof(body) - 1);
        body[sizeof(body)-1] = '\0';
    }

    char response[BUFFER_SIZE] = {0};

    // POST /visit
    if (strcmp(path, "/visit") == 0 && strcmp(method, "POST") == 0) {
        char url[MAX_URL_LENGTH] = {0};
        char user[256] = {0};
        parse_post_body(body, url, sizeof(url), user, sizeof(user));
        if (strlen(url) == 0 || strlen(user) == 0) {
            send_response(client_socket, "400 Bad Request", "application/json", "{\"error\":\"Missing url or user\"}");
            return;
        }
        add_visit_to_db(user, url);
        snprintf(response, sizeof(response), "{\"status\":\"success\",\"url\":\"%s\"}", url);
        send_response(client_socket, "200 OK", "application/json", response);
        return;
    }

    // POST /back
    if (strcmp(path, "/back") == 0 && strcmp(method, "POST") == 0) {
        char url_tmp[MAX_URL_LENGTH] = {0};
        char user[256] = {0};
        parse_post_body(body, url_tmp, sizeof(url_tmp), user, sizeof(user));
        if (strlen(user) == 0) {
            send_response(client_socket, "400 Bad Request", "application/json", "{\"error\":\"Missing user\"}");
            return;
        }
        char out_url[MAX_URL_LENGTH] = {0};
        if (go_back_for_user(user, out_url, sizeof(out_url))) {
            snprintf(response, sizeof(response), "{\"status\":\"success\",\"url\":\"%s\"}", out_url);
        } else {
            strcpy(response, "{\"status\":\"error\",\"message\":\"No previous page\"}");
        }
        send_response(client_socket, "200 OK", "application/json", response);
        return;
    }

    // POST /forward
    if (strcmp(path, "/forward") == 0 && strcmp(method, "POST") == 0) {
        char url_tmp[MAX_URL_LENGTH] = {0};
        char user[256] = {0};
        parse_post_body(body, url_tmp, sizeof(url_tmp), user, sizeof(user));
        if (strlen(user) == 0) {
            send_response(client_socket, "400 Bad Request", "application/json", "{\"error\":\"Missing user\"}");
            return;
        }
        char out_url[MAX_URL_LENGTH] = {0};
        if (go_forward_for_user(user, out_url, sizeof(out_url))) {
            snprintf(response, sizeof(response), "{\"status\":\"success\",\"url\":\"%s\"}", out_url);
        } else {
            strcpy(response, "{\"status\":\"error\",\"message\":\"No next page\"}");
        }
        send_response(client_socket, "200 OK", "application/json", response);
        return;
    }

    // POST /clear
    if (strcmp(path, "/clear") == 0 && strcmp(method, "POST") == 0) {
        char url_tmp[MAX_URL_LENGTH] = {0};
        char user[256] = {0};
        parse_post_body(body, url_tmp, sizeof(url_tmp), user, sizeof(user));
        if (strlen(user) == 0) {
            send_response(client_socket, "400 Bad Request", "application/json", "{\"error\":\"Missing user\"}");
            return;
        }
        clear_history_for_user(user);
        send_response(client_socket, "200 OK", "application/json", "{\"status\":\"success\"}");
        return;
    }

    // GET /history?user=...
    if (strncmp(path, "/history", 8) == 0 && strcmp(method, "GET") == 0) {
        char *q = strchr(path, '?');
        char user[256] = {0};
        if (q) {
            char params[512];
            strncpy(params, q+1, sizeof(params)-1);
            params[sizeof(params)-1] = '\0';
            char *p = strtok(params, "&");
            while (p) {
                if (strncmp(p, "user=", 5) == 0) {
                    url_decode(user, p + 5);
                }
                p = strtok(NULL, "&");
            }
        }
        if (strlen(user) == 0) {
            send_response(client_socket, "400 Bad Request", "application/json", "{\"error\":\"Missing user query\"}");
            return;
        }
        get_history_json_for_user(user, response, sizeof(response));
        send_response(client_socket, "200 OK", "application/json", response);
        return;
    }

    // GET /search?q=...&user=...
    if (strncmp(path, "/search", 7) == 0 && strcmp(method, "GET") == 0) {
        char *q = strchr(path, '?');
        char query[512] = {0};
        char user[256] = {0};
        if (q) {
            char params[1024];
            strncpy(params, q+1, sizeof(params)-1);
            params[sizeof(params)-1] = '\0';
            char *p = strtok(params, "&");
            while (p) {
                if (strncmp(p, "q=", 2) == 0) {
                    url_decode(query, p + 2);
                } else if (strncmp(p, "user=", 5) == 0) {
                    url_decode(user, p + 5);
                }
                p = strtok(NULL, "&");
            }
        }
        if (strlen(user) == 0) {
            send_response(client_socket, "400 Bad Request", "application/json", "{\"error\":\"Missing user query\"}");
            return;
        }
        search_history_for_user(user, query, response, sizeof(response));
        send_response(client_socket, "200 OK", "application/json", response);
        return;
    }

    // Bookmarks endpoints
    if (strncmp(path, "/bookmarks", 10) == 0) {
        // GET /bookmarks?user=...
        if (strcmp(method, "GET") == 0) {
            char *q = strchr(path, '?');
            char user[256] = {0};
            if (q) {
                char params[512];
                strncpy(params, q+1, sizeof(params)-1);
                params[sizeof(params)-1] = '\0';
                char *p = strtok(params, "&");
                while (p) {
                    if (strncmp(p, "user=", 5) == 0) {
                        url_decode(user, p + 5);
                    }
                    p = strtok(NULL, "&");
                }
            }
            if (strlen(user) == 0) {
                send_response(client_socket, "400 Bad Request", "application/json", "{\"error\":\"Missing user\"}");
                return;
            }
            get_bookmarks_json(user, response, sizeof(response));
            send_response(client_socket, "200 OK", "application/json", response);
            return;
        }
        // POST /bookmarks (add)
        if (strcmp(method, "POST") == 0) {
            char url[MAX_URL_LENGTH] = {0};
            char user[256] = {0};
            parse_post_body(body, url, sizeof(url), user, sizeof(user));
            if (strlen(user) == 0 || strlen(url) == 0) {
                send_response(client_socket, "400 Bad Request", "application/json", "{\"error\":\"Missing user or url\"}");
                return;
            }
            add_bookmark_to_db(user, url);
            snprintf(response, sizeof(response), "{\"status\":\"success\",\"url\":\"%s\"}", url);
            send_response(client_socket, "200 OK", "application/json", response);
            return;
        }
        // DELETE /bookmarks?user=...&url=...
        if (strcmp(method, "DELETE") == 0) {
            char *q = strchr(path, '?');
            char user[256] = {0};
            char url[MAX_URL_LENGTH] = {0};
            if (q) {
                char params[2048];
                strncpy(params, q+1, sizeof(params)-1);
                params[sizeof(params)-1] = '\0';
                char *p = strtok(params, "&");
                while (p) {
                    if (strncmp(p, "user=", 5) == 0) url_decode(user, p + 5);
                    else if (strncmp(p, "url=", 4) == 0) url_decode(url, p + 4);
                    p = strtok(NULL, "&");
                }
            }
            if (strlen(user) == 0 || strlen(url) == 0) {
                send_response(client_socket, "400 Bad Request", "application/json", "{\"error\":\"Missing user or url\"}");
                return;
            }
            remove_bookmark_from_db(user, url);
            send_response(client_socket, "200 OK", "application/json", "{\"status\":\"success\"}");
            return;
        }
    }

    // Fallback 404
    send_response(client_socket, "404 Not Found", "text/plain", "404 Not Found");
}

// -------------------- Main --------------------
int main() {
    SOCKET server_socket, client_socket;
    struct sockaddr_in server, client;
    int client_len;
    char buffer[BUFFER_SIZE];

    printf("Browser History Management Server (SQLite)\n");
    printf("=========================================\n\n");

    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        printf("Failed to initialize Winsock. Error Code: %d\n", WSAGetLastError());
        return 1;
    }

    if (init_db() != SQLITE_OK) {
        printf("Failed to initialize database.\n");
        return 1;
    }

    if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("Could not create socket. Error Code: %d\n", WSAGetLastError());
        return 1;
    }

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr *)&server, sizeof(server)) == SOCKET_ERROR) {
        printf("Bind failed. Error Code: %d\n", WSAGetLastError());
        return 1;
    }

    listen(server_socket, 5);
    printf("Server listening on port %d...\n", PORT);

    client_len = sizeof(struct sockaddr_in);
    while (1) {
        client_socket = accept(server_socket, (struct sockaddr *)&client, &client_len);
        if (client_socket == INVALID_SOCKET) {
            printf("Accept failed. Error Code: %d\n", WSAGetLastError());
            continue;
        }

        int recv_size = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (recv_size > 0) {
            buffer[recv_size] = '\0';
            // printf("Request received:\n%s\n", buffer);
            handle_request(client_socket, buffer);
        }

        closesocket(client_socket);
    }

    closesocket(server_socket);
    sqlite3_close(db);
    WSACleanup();
    return 0;
}
