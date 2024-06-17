#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <time.h>

#define BUFFER_SIZE 1024
#define LOG_FILE "55.log"

// Structure pour stocker les valeurs de configuration
struct ServerConfig
{
    char document_root[BUFFER_SIZE];
    int port;
};

// Fonction pour lire la configuration à partir du fichier .conf
struct ServerConfig read_config(const char *config_file)
{
    struct ServerConfig config;
    FILE *file = fopen(config_file, "r");
    if (file == NULL)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }
    fscanf(file, "DocumentRoot %s\n", config.document_root);
    fscanf(file, "Port %d\n", &config.port);
    fclose(file);
    return config;
}

// Fonction pour enregistrer les erreurs via Syslog
void log_error(const char *message)
{
    openlog("HTTPServer", LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_ERR, "%s", message);
    closelog();
}

void handle_client(int client_socket, const char *document_root, const char *client_ip)
{
    char buffer[BUFFER_SIZE];
    int bytes_read;

    // Lire la requête du client
    bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);
    if (bytes_read < 0)
    {
        perror("read");
        close(client_socket);
        return;
    }

    buffer[bytes_read] = '\0'; // Terminer la chaîne de caractères

    // Analyser la requête HTTP pour obtenir le chemin du fichier demandé
    char file_path[BUFFER_SIZE];
    sscanf(buffer, "GET %s", file_path);

    // Si aucun chemin n'est spécifié, retourner "index.html"
    if (strcmp(file_path, "/") == 0)
    {
        snprintf(file_path, sizeof(file_path), "/index.html");
    }

    // Construire le chemin complet du fichier demandé
    char full_path[BUFFER_SIZE];
    snprintf(full_path, sizeof(full_path), "%s%s", document_root, file_path);

    // Ouvrir le fichier demandé
    int file_fd = open(full_path, O_RDONLY);
    if (file_fd < 0)
    {
        // Le fichier demandé n'existe pas, envoyer une réponse 404
        const char *not_found_response =
            "HTTP/1.0 404 Not Found\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 100\r\n"
            "\r\n"
            "<html><body><h1>404 Not Found</h1><p>The requested file could not be found.</p></body></html>";
        write(client_socket, not_found_response, strlen(not_found_response));
        close(client_socket);
        log_error("Requested file not found");
        return;
    }

    // Déterminer le type de contenu en fonction de l'extension du fichier
    const char *content_type;
    if (strstr(file_path, ".html") != NULL)
    {
        content_type = "text/html";
    }
    else if (strstr(file_path, ".css") != NULL)
    {
        content_type = "text/css";
    }
    else
    {
        content_type = "text/plain"; // Par défaut, texte brut si le type de contenu ne peut être déterminé
    }

    // Lire le contenu du fichier et l'envoyer au client
    struct stat file_stat;
    fstat(file_fd, &file_stat);
    const char *ok_response =
        "HTTP/1.0 200 OK\r\n";
    write(client_socket, ok_response, strlen(ok_response));

    // Envoyer l'en-tête Content-Type
    char content_type_header[BUFFER_SIZE];
    snprintf(content_type_header, sizeof(content_type_header), "Content-Type: %s\r\n", content_type);
    write(client_socket, content_type_header, strlen(content_type_header));

    // Fin des en-têtes
    write(client_socket, "\r\n", 2);

    // Envoyer le contenu du fichier
    char file_content[BUFFER_SIZE];
    ssize_t bytes_read_from_file;
    while ((bytes_read_from_file = read(file_fd, file_content, BUFFER_SIZE)) > 0)
    {
        write(client_socket, file_content, bytes_read_from_file);
    }

    // Fermer le fichier et le socket client
    close(file_fd);
    close(client_socket);

    // Enregistrer la requête dans le fichier journal
    FILE *log_file = fopen(LOG_FILE, "a");
    if (log_file != NULL)
    {
        // Obtenir la date et l'heure actuelles
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

        // Écrire l'entrée de journal
        fprintf(log_file, "%s - %s - %s\n", time_str, client_ip, file_path);

        // Fermer le fichier journal
        fclose(log_file);
    }
    else
    {
        perror("fopen");
        log_error("Unable to open log file");
    }
}

void sigchld_handler(int sig)
{
    // Récolter tous les processus zombies
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

int main(int argc, char *argv[])
{
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Ignorer les signaux SIGCHLD pour éviter les processus zombies
    signal(SIGCHLD, sigchld_handler);

    // Nom du fichier de configuration par défaut
    const char *default_config_file = "55.conf";
    struct ServerConfig config;

    // Vérifier si un fichier de configuration est spécifié en argument
    if (argc == 2)
    {
        // Lire la configuration à partir du fichier .conf spécifié
        config = read_config(argv[1]);
    }
    else if (argc == 1)
    {
        // Lire la configuration à partir du fichier de configuration par défaut
        config = read_config(default_config_file);
    }
    else
    {
        fprintf(stderr, "Usage: %s [config_file]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Afficher le message de démarrage
    printf("Serveur HTTP en écoute sur le port %d...\n", config.port);

    // Devenir un démon
    if (fork() != 0)
    {
        exit(0); // Terminer le processus parent
    }
    setsid(); // Créer une nouvelle session

    // Créer le socket TCP
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        perror("socket");
        log_error("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Configurer l'adresse du serveur
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(config.port);

    // Associer le socket à l'adresse et au port
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_socket);
        log_error("Bind failed");
        exit(EXIT_FAILURE);
    }

    // Mettre le socket en mode écoute
    if (listen(server_socket, 10) < 0)
    {
        perror("listen");
        close(server_socket);
        log_error("Listen failed");
        exit(EXIT_FAILURE);
    }

    syslog(LOG_NOTICE, "HTTP server started on port %d", config.port);

    while (1)
    {
        // Accepter une nouvelle connexion
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket < 0)
        {
            perror("accept");
            close(server_socket);
            log_error("Accept failed");
            exit(EXIT_FAILURE);
        }

        // Récupérer l'adresse IP du client
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        // Créer un processus enfant pour traiter la requête du client
        if (fork() == 0)
        {
            // Processus enfant
            close(server_socket); // Le processus enfant n'a pas besoin du socket d'écoute
            handle_client(client_socket, config.document_root, client_ip);
            exit(EXIT_SUCCESS);
        }
        close(client_socket); // Le processus parent n'a pas besoin du socket de communication
    }

    // Fermer le socket du serveur
    close(server_socket);
    return 0;
}
