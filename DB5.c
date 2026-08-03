#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define DBFILE "Database.txt"
#define MAXLINE 1024

void createDatabase(void);
void cmdNew(char *data);
void cmdFind(char *text);
void cmdShow(int record);
void cmdDelete(int record);
void cmdList(void);
void cmdSeek(char *text);

int containsIgnoreCase(const char *haystack, const char *needle);
int startsWithIgnoreCase(const char *text, const char *prefix)
{
    while (*prefix)
    {
        if (*text == '\0')
            return 0;

        if (tolower((unsigned char)*text) !=
            tolower((unsigned char)*prefix))
            return 0;

        text++;
        prefix++;
    }

    return 1;
}

int main(void)
{
    char line[MAXLINE];

    createDatabase();

    printf("Simple Database\n");
    printf("Commands:\n");
    printf("  New text\n");
    printf("  Find text\n");
    printf("  Show number\n");
    printf("  Delete number\n");
    printf("  List\n");
    printf("  Exit\n\n");

    while (1)
    {
        printf("> ");

        if (!fgets(line, sizeof(line), stdin))
            break;

        line[strcspn(line, "\r\n")] = 0;

        if (_strnicmp(line, "New ", 4) == 0)
            cmdNew(line + 4);

        else if (_strnicmp(line, "Find ", 5) == 0)
            cmdFind(line + 5);
        else if (_strnicmp(line, "Seek ", 5) == 0)
            cmdSeek(line + 5);
        else if (_strnicmp(line, "Show ", 5) == 0)
            cmdShow(atoi(line + 5));

        else if (_strnicmp(line, "Delete ", 7) == 0)
            cmdDelete(atoi(line + 7));

        else if (_stricmp(line, "List") == 0)
            cmdList();

        else if (_stricmp(line, "Exit") == 0)
            break;
        
        else
            printf("Unknown command\n");
    }

    return 0;
}

void createDatabase(void)
{
    FILE *fp = fopen(DBFILE, "r");

    if (fp == NULL)
    {
        fp = fopen(DBFILE, "w");
        if (fp)
            fclose(fp);
    }
    else
        fclose(fp);
}

int containsIgnoreCase(const char *haystack, const char *needle)
{
    char h[MAXLINE];
    char n[MAXLINE];

    strcpy(h, haystack);
    strcpy(n, needle);

    _strlwr(h);
    _strlwr(n);

    return strstr(h, n) != NULL;
}

void cmdNew(char *data)
{
    FILE *fp;
    char line[MAXLINE];
    int highest = 0;
    int rec;

    fp = fopen(DBFILE, "r");

    while (fgets(line, sizeof(line), fp))
    {
        if (sscanf(line, "%d >", &rec) == 1)
        {
            if (rec > highest)
                highest = rec;
        }
    }

    fclose(fp);

    fp = fopen(DBFILE, "a");

    fprintf(fp, "%d > %s\n", highest + 1, data);

    fclose(fp);

    printf("Added record %d\n", highest + 1);
}

void cmdFind(char *text)
{
    FILE *fp;
    char line[MAXLINE];
    int rec;
    char data[MAXLINE];
    int first = 1;

    fp = fopen(DBFILE, "r");

    while (fgets(line, sizeof(line), fp))
    {
        if (sscanf(line, "%d > %[^\n]", &rec, data) == 2)
        {
            if (containsIgnoreCase(data, text))
            {
                if (!first)
                    printf(",");

                printf("%d", rec);
                first = 0;
            }
        }
    }

    fclose(fp);

    printf("\n");
}

void cmdSeek(char *text)
{
    FILE *fp;
    char line[MAXLINE];
    int rec;
    char data[MAXLINE];
    int first = 1;

    fp = fopen(DBFILE, "r");

    while (fgets(line, sizeof(line), fp))
    {
        if (sscanf(line, "%d > %[^\n]", &rec, data) == 2)
        {
            if (startsWithIgnoreCase(data, text))
            {
                if (!first)
                    printf(",");

                printf("%d", rec);
                first = 0;
            }
        }
    }

    fclose(fp);

    printf("\n");
}

void cmdShow(int wanted)
{
    FILE *fp;
    char line[MAXLINE];
    int rec;
    char data[MAXLINE];

    fp = fopen(DBFILE, "r");

    while (fgets(line, sizeof(line), fp))
    {
        if (sscanf(line, "%d > %[^\n]", &rec, data) == 2)
        {
            if (rec == wanted)
            {
                printf("%d > %s\n", rec, data);
                fclose(fp);
                return;
            }
        }
    }

    fclose(fp);

    printf("Record not found\n");
}

void cmdDelete(int wanted)
{
    FILE *in;
    FILE *out;
    char line[MAXLINE];
    int rec;
    char data[MAXLINE];
    int found = 0;

    in = fopen(DBFILE, "r");
    out = fopen("Database.tmp", "w");

    while (fgets(line, sizeof(line), in))
    {
        if (sscanf(line, "%d > %[^\n]", &rec, data) == 2)
        {
            if (rec == wanted)
            {
                found = 1;
                continue;
            }

            fprintf(out, "%d > %s\n", rec, data);
        }
    }

    fclose(in);
    fclose(out);

    remove(DBFILE);
    rename("Database.tmp", DBFILE);

    if (found)
        printf("Deleted\n");
    else
        printf("Record not found\n");
}

void cmdList(void)
{
    FILE *fp;
    char line[MAXLINE];

    fp = fopen(DBFILE, "r");

    while (fgets(line, sizeof(line), fp))
        printf("%s", line);

    fclose(fp);
}