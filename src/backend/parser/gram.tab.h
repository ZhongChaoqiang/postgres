/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_BASE_YY_GRAM_TAB_H_INCLUDED
# define YY_BASE_YY_GRAM_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int base_yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    IDENT = 258,                   /* IDENT  */
    UIDENT = 259,                  /* UIDENT  */
    FCONST = 260,                  /* FCONST  */
    SCONST = 261,                  /* SCONST  */
    USCONST = 262,                 /* USCONST  */
    BCONST = 263,                  /* BCONST  */
    XCONST = 264,                  /* XCONST  */
    Op = 265,                      /* Op  */
    ICONST = 266,                  /* ICONST  */
    PARAM = 267,                   /* PARAM  */
    TYPECAST = 268,                /* TYPECAST  */
    DOT_DOT = 269,                 /* DOT_DOT  */
    COLON_EQUALS = 270,            /* COLON_EQUALS  */
    EQUALS_GREATER = 271,          /* EQUALS_GREATER  */
    LESS_EQUALS = 272,             /* LESS_EQUALS  */
    GREATER_EQUALS = 273,          /* GREATER_EQUALS  */
    NOT_EQUALS = 274,              /* NOT_EQUALS  */
    ABORT_P = 275,                 /* ABORT_P  */
    ABSENT = 276,                  /* ABSENT  */
    ABSOLUTE_P = 277,              /* ABSOLUTE_P  */
    ACCESS = 278,                  /* ACCESS  */
    ACTION = 279,                  /* ACTION  */
    ADD_P = 280,                   /* ADD_P  */
    ADMIN = 281,                   /* ADMIN  */
    AFTER = 282,                   /* AFTER  */
    AGGREGATE = 283,               /* AGGREGATE  */
    ALL = 284,                     /* ALL  */
    ALSO = 285,                    /* ALSO  */
    ALTER = 286,                   /* ALTER  */
    ALWAYS = 287,                  /* ALWAYS  */
    ANALYSE = 288,                 /* ANALYSE  */
    ANALYZE = 289,                 /* ANALYZE  */
    AND = 290,                     /* AND  */
    ANY = 291,                     /* ANY  */
    ARRAY = 292,                   /* ARRAY  */
    AS = 293,                      /* AS  */
    ASC = 294,                     /* ASC  */
    ASENSITIVE = 295,              /* ASENSITIVE  */
    ASSERTION = 296,               /* ASSERTION  */
    ASSIGNMENT = 297,              /* ASSIGNMENT  */
    ASYMMETRIC = 298,              /* ASYMMETRIC  */
    ATOMIC = 299,                  /* ATOMIC  */
    AT = 300,                      /* AT  */
    ATTACH = 301,                  /* ATTACH  */
    ATTRIBUTE = 302,               /* ATTRIBUTE  */
    AUTHORIZATION = 303,           /* AUTHORIZATION  */
    BACKWARD = 304,                /* BACKWARD  */
    BEFORE = 305,                  /* BEFORE  */
    BEGIN_P = 306,                 /* BEGIN_P  */
    BETWEEN = 307,                 /* BETWEEN  */
    BIGINT = 308,                  /* BIGINT  */
    BINARY = 309,                  /* BINARY  */
    BIT = 310,                     /* BIT  */
    BOOLEAN_P = 311,               /* BOOLEAN_P  */
    BOTH = 312,                    /* BOTH  */
    BREADTH = 313,                 /* BREADTH  */
    BY = 314,                      /* BY  */
    CACHE = 315,                   /* CACHE  */
    CALL = 316,                    /* CALL  */
    CALLED = 317,                  /* CALLED  */
    CASCADE = 318,                 /* CASCADE  */
    CASCADED = 319,                /* CASCADED  */
    CASE = 320,                    /* CASE  */
    CAST = 321,                    /* CAST  */
    CATALOG_P = 322,               /* CATALOG_P  */
    CHAIN = 323,                   /* CHAIN  */
    CHAR_P = 324,                  /* CHAR_P  */
    CHARACTER = 325,               /* CHARACTER  */
    CHARACTERISTICS = 326,         /* CHARACTERISTICS  */
    CHECK = 327,                   /* CHECK  */
    CHECKPOINT = 328,              /* CHECKPOINT  */
    CLASS = 329,                   /* CLASS  */
    CLOSE = 330,                   /* CLOSE  */
    CLUSTER = 331,                 /* CLUSTER  */
    COALESCE = 332,                /* COALESCE  */
    COLLATE = 333,                 /* COLLATE  */
    COLLATION = 334,               /* COLLATION  */
    COLUMN = 335,                  /* COLUMN  */
    COLUMNS = 336,                 /* COLUMNS  */
    COMMENT = 337,                 /* COMMENT  */
    COMMENTS = 338,                /* COMMENTS  */
    COMMIT = 339,                  /* COMMIT  */
    COMMITTED = 340,               /* COMMITTED  */
    COMPRESSION = 341,             /* COMPRESSION  */
    CONCURRENTLY = 342,            /* CONCURRENTLY  */
    CONDITIONAL = 343,             /* CONDITIONAL  */
    CONFIGURATION = 344,           /* CONFIGURATION  */
    CONFLICT = 345,                /* CONFLICT  */
    CONNECTION = 346,              /* CONNECTION  */
    CONSTRAINT = 347,              /* CONSTRAINT  */
    CONSTRAINTS = 348,             /* CONSTRAINTS  */
    CONTENT_P = 349,               /* CONTENT_P  */
    CONTINUE_P = 350,              /* CONTINUE_P  */
    CONVERSION_P = 351,            /* CONVERSION_P  */
    COPY = 352,                    /* COPY  */
    COST = 353,                    /* COST  */
    CREATE = 354,                  /* CREATE  */
    CROSS = 355,                   /* CROSS  */
    CSV = 356,                     /* CSV  */
    CUBE = 357,                    /* CUBE  */
    CURRENT_P = 358,               /* CURRENT_P  */
    CURRENT_CATALOG = 359,         /* CURRENT_CATALOG  */
    CURRENT_DATE = 360,            /* CURRENT_DATE  */
    CURRENT_ROLE = 361,            /* CURRENT_ROLE  */
    CURRENT_SCHEMA = 362,          /* CURRENT_SCHEMA  */
    CURRENT_TIME = 363,            /* CURRENT_TIME  */
    CURRENT_TIMESTAMP = 364,       /* CURRENT_TIMESTAMP  */
    CURRENT_USER = 365,            /* CURRENT_USER  */
    CURSOR = 366,                  /* CURSOR  */
    CYCLE = 367,                   /* CYCLE  */
    DATA_P = 368,                  /* DATA_P  */
    DATABASE = 369,                /* DATABASE  */
    DAY_P = 370,                   /* DAY_P  */
    DEALLOCATE = 371,              /* DEALLOCATE  */
    DEC = 372,                     /* DEC  */
    DECIMAL_P = 373,               /* DECIMAL_P  */
    DECLARE = 374,                 /* DECLARE  */
    DEFAULT = 375,                 /* DEFAULT  */
    DEFAULTS = 376,                /* DEFAULTS  */
    DEFERRABLE = 377,              /* DEFERRABLE  */
    DEFERRED = 378,                /* DEFERRED  */
    DEFINER = 379,                 /* DEFINER  */
    DELETE_P = 380,                /* DELETE_P  */
    DELIMITER = 381,               /* DELIMITER  */
    DELIMITERS = 382,              /* DELIMITERS  */
    DEPENDS = 383,                 /* DEPENDS  */
    DEPTH = 384,                   /* DEPTH  */
    DESC = 385,                    /* DESC  */
    DETACH = 386,                  /* DETACH  */
    DICTIONARY = 387,              /* DICTIONARY  */
    DISABLE_P = 388,               /* DISABLE_P  */
    DISCARD = 389,                 /* DISCARD  */
    DISTINCT = 390,                /* DISTINCT  */
    DO = 391,                      /* DO  */
    DOCUMENT_P = 392,              /* DOCUMENT_P  */
    DOMAIN_P = 393,                /* DOMAIN_P  */
    DOUBLE_P = 394,                /* DOUBLE_P  */
    DROP = 395,                    /* DROP  */
    EACH = 396,                    /* EACH  */
    ELSE = 397,                    /* ELSE  */
    EMBEDDING = 398,               /* EMBEDDING  */
    EMPTY_P = 399,                 /* EMPTY_P  */
    ENABLE_P = 400,                /* ENABLE_P  */
    ENCODING = 401,                /* ENCODING  */
    ENCRYPTED = 402,               /* ENCRYPTED  */
    END_P = 403,                   /* END_P  */
    ENFORCED = 404,                /* ENFORCED  */
    ENUM_P = 405,                  /* ENUM_P  */
    ERROR_P = 406,                 /* ERROR_P  */
    ESCAPE = 407,                  /* ESCAPE  */
    EVENT = 408,                   /* EVENT  */
    EXCEPT = 409,                  /* EXCEPT  */
    EXCLUDE = 410,                 /* EXCLUDE  */
    EXCLUDING = 411,               /* EXCLUDING  */
    EXCLUSIVE = 412,               /* EXCLUSIVE  */
    EXECUTE = 413,                 /* EXECUTE  */
    EXISTS = 414,                  /* EXISTS  */
    EXPLAIN = 415,                 /* EXPLAIN  */
    EXPRESSION = 416,              /* EXPRESSION  */
    EXTENSION = 417,               /* EXTENSION  */
    EXTERNAL = 418,                /* EXTERNAL  */
    EXTRACT = 419,                 /* EXTRACT  */
    FALSE_P = 420,                 /* FALSE_P  */
    FAMILY = 421,                  /* FAMILY  */
    FETCH = 422,                   /* FETCH  */
    FILTER = 423,                  /* FILTER  */
    FINALIZE = 424,                /* FINALIZE  */
    FIRST_P = 425,                 /* FIRST_P  */
    FLOAT_P = 426,                 /* FLOAT_P  */
    FOLLOWING = 427,               /* FOLLOWING  */
    FOR = 428,                     /* FOR  */
    FORCE = 429,                   /* FORCE  */
    FOREIGN = 430,                 /* FOREIGN  */
    FORMAT = 431,                  /* FORMAT  */
    FORWARD = 432,                 /* FORWARD  */
    FREEZE = 433,                  /* FREEZE  */
    FROM = 434,                    /* FROM  */
    FULL = 435,                    /* FULL  */
    FUNCTION = 436,                /* FUNCTION  */
    FUNCTIONS = 437,               /* FUNCTIONS  */
    GENERATED = 438,               /* GENERATED  */
    GLOBAL = 439,                  /* GLOBAL  */
    GRANT = 440,                   /* GRANT  */
    GRANTED = 441,                 /* GRANTED  */
    GREATEST = 442,                /* GREATEST  */
    GROUP_P = 443,                 /* GROUP_P  */
    GROUPING = 444,                /* GROUPING  */
    GROUPS = 445,                  /* GROUPS  */
    HANDLER = 446,                 /* HANDLER  */
    HAVING = 447,                  /* HAVING  */
    HEADER_P = 448,                /* HEADER_P  */
    HOLD = 449,                    /* HOLD  */
    HOUR_P = 450,                  /* HOUR_P  */
    IDENTITY_P = 451,              /* IDENTITY_P  */
    IF_P = 452,                    /* IF_P  */
    ILIKE = 453,                   /* ILIKE  */
    IMMEDIATE = 454,               /* IMMEDIATE  */
    IMMUTABLE = 455,               /* IMMUTABLE  */
    IMPLICIT_P = 456,              /* IMPLICIT_P  */
    IMPORT_P = 457,                /* IMPORT_P  */
    IN_P = 458,                    /* IN_P  */
    INCLUDE = 459,                 /* INCLUDE  */
    INCLUDING = 460,               /* INCLUDING  */
    INCREMENT = 461,               /* INCREMENT  */
    INDENT = 462,                  /* INDENT  */
    INDEX = 463,                   /* INDEX  */
    INDEXES = 464,                 /* INDEXES  */
    INHERIT = 465,                 /* INHERIT  */
    INHERITS = 466,                /* INHERITS  */
    INITIALLY = 467,               /* INITIALLY  */
    INLINE_P = 468,                /* INLINE_P  */
    INNER_P = 469,                 /* INNER_P  */
    INOUT = 470,                   /* INOUT  */
    INPUT_P = 471,                 /* INPUT_P  */
    INSENSITIVE = 472,             /* INSENSITIVE  */
    INSERT = 473,                  /* INSERT  */
    INSTEAD = 474,                 /* INSTEAD  */
    INT_P = 475,                   /* INT_P  */
    INTEGER = 476,                 /* INTEGER  */
    INTERSECT = 477,               /* INTERSECT  */
    INTERVAL = 478,                /* INTERVAL  */
    INTO = 479,                    /* INTO  */
    INVOKER = 480,                 /* INVOKER  */
    IS = 481,                      /* IS  */
    ISNULL = 482,                  /* ISNULL  */
    ISOLATION = 483,               /* ISOLATION  */
    JOIN = 484,                    /* JOIN  */
    JSON = 485,                    /* JSON  */
    JSON_ARRAY = 486,              /* JSON_ARRAY  */
    JSON_ARRAYAGG = 487,           /* JSON_ARRAYAGG  */
    JSON_EXISTS = 488,             /* JSON_EXISTS  */
    JSON_OBJECT = 489,             /* JSON_OBJECT  */
    JSON_OBJECTAGG = 490,          /* JSON_OBJECTAGG  */
    JSON_QUERY = 491,              /* JSON_QUERY  */
    JSON_SCALAR = 492,             /* JSON_SCALAR  */
    JSON_SERIALIZE = 493,          /* JSON_SERIALIZE  */
    JSON_TABLE = 494,              /* JSON_TABLE  */
    JSON_VALUE = 495,              /* JSON_VALUE  */
    KEEP = 496,                    /* KEEP  */
    KEY = 497,                     /* KEY  */
    KEYS = 498,                    /* KEYS  */
    LABEL = 499,                   /* LABEL  */
    LANGUAGE = 500,                /* LANGUAGE  */
    LARGE_P = 501,                 /* LARGE_P  */
    LAST_P = 502,                  /* LAST_P  */
    LATERAL_P = 503,               /* LATERAL_P  */
    LEADING = 504,                 /* LEADING  */
    LEAKPROOF = 505,               /* LEAKPROOF  */
    LEAST = 506,                   /* LEAST  */
    LEFT = 507,                    /* LEFT  */
    LEVEL = 508,                   /* LEVEL  */
    LIKE = 509,                    /* LIKE  */
    LIMIT = 510,                   /* LIMIT  */
    LISTEN = 511,                  /* LISTEN  */
    LOAD = 512,                    /* LOAD  */
    LOCAL = 513,                   /* LOCAL  */
    LOCALTIME = 514,               /* LOCALTIME  */
    LOCALTIMESTAMP = 515,          /* LOCALTIMESTAMP  */
    LOCATION = 516,                /* LOCATION  */
    LOCK_P = 517,                  /* LOCK_P  */
    LOCKED = 518,                  /* LOCKED  */
    LOGGED = 519,                  /* LOGGED  */
    MAPPING = 520,                 /* MAPPING  */
    MATCH = 521,                   /* MATCH  */
    MATCHED = 522,                 /* MATCHED  */
    MATERIALIZED = 523,            /* MATERIALIZED  */
    MAXVALUE = 524,                /* MAXVALUE  */
    MERGE = 525,                   /* MERGE  */
    MERGE_ACTION = 526,            /* MERGE_ACTION  */
    METHOD = 527,                  /* METHOD  */
    MINUTE_P = 528,                /* MINUTE_P  */
    MINVALUE = 529,                /* MINVALUE  */
    MODE = 530,                    /* MODE  */
    MONTH_P = 531,                 /* MONTH_P  */
    MOVE = 532,                    /* MOVE  */
    NAME_P = 533,                  /* NAME_P  */
    NAMES = 534,                   /* NAMES  */
    NATIONAL = 535,                /* NATIONAL  */
    NATURAL = 536,                 /* NATURAL  */
    NCHAR = 537,                   /* NCHAR  */
    NESTED = 538,                  /* NESTED  */
    NEW = 539,                     /* NEW  */
    NEXT = 540,                    /* NEXT  */
    NFC = 541,                     /* NFC  */
    NFD = 542,                     /* NFD  */
    NFKC = 543,                    /* NFKC  */
    NFKD = 544,                    /* NFKD  */
    NO = 545,                      /* NO  */
    NONE = 546,                    /* NONE  */
    NORMALIZE = 547,               /* NORMALIZE  */
    NORMALIZED = 548,              /* NORMALIZED  */
    NOT = 549,                     /* NOT  */
    NOTHING = 550,                 /* NOTHING  */
    NOTIFY = 551,                  /* NOTIFY  */
    NOTNULL = 552,                 /* NOTNULL  */
    NOWAIT = 553,                  /* NOWAIT  */
    NULL_P = 554,                  /* NULL_P  */
    NULLIF = 555,                  /* NULLIF  */
    NULLS_P = 556,                 /* NULLS_P  */
    NUMERIC = 557,                 /* NUMERIC  */
    OBJECT_P = 558,                /* OBJECT_P  */
    OBJECTS_P = 559,               /* OBJECTS_P  */
    OF = 560,                      /* OF  */
    OFF = 561,                     /* OFF  */
    OFFSET = 562,                  /* OFFSET  */
    OIDS = 563,                    /* OIDS  */
    OLD = 564,                     /* OLD  */
    OMIT = 565,                    /* OMIT  */
    ON = 566,                      /* ON  */
    ONLY = 567,                    /* ONLY  */
    OPERATOR = 568,                /* OPERATOR  */
    OPTION = 569,                  /* OPTION  */
    OPTIONS = 570,                 /* OPTIONS  */
    OR = 571,                      /* OR  */
    ORDER = 572,                   /* ORDER  */
    ORDINALITY = 573,              /* ORDINALITY  */
    OTHERS = 574,                  /* OTHERS  */
    OUT_P = 575,                   /* OUT_P  */
    OUTER_P = 576,                 /* OUTER_P  */
    OVER = 577,                    /* OVER  */
    OVERLAPS = 578,                /* OVERLAPS  */
    OVERLAY = 579,                 /* OVERLAY  */
    OVERRIDING = 580,              /* OVERRIDING  */
    OWNED = 581,                   /* OWNED  */
    OWNER = 582,                   /* OWNER  */
    PARALLEL = 583,                /* PARALLEL  */
    PARAMETER = 584,               /* PARAMETER  */
    PARSER = 585,                  /* PARSER  */
    PARTIAL = 586,                 /* PARTIAL  */
    PARTITION = 587,               /* PARTITION  */
    PASSING = 588,                 /* PASSING  */
    PASSWORD = 589,                /* PASSWORD  */
    PATH = 590,                    /* PATH  */
    PERIOD = 591,                  /* PERIOD  */
    PLACING = 592,                 /* PLACING  */
    PLAN = 593,                    /* PLAN  */
    PLANS = 594,                   /* PLANS  */
    POLICY = 595,                  /* POLICY  */
    POSITION = 596,                /* POSITION  */
    PRECEDING = 597,               /* PRECEDING  */
    PRECISION = 598,               /* PRECISION  */
    PRESERVE = 599,                /* PRESERVE  */
    PREPARE = 600,                 /* PREPARE  */
    PREPARED = 601,                /* PREPARED  */
    PRIMARY = 602,                 /* PRIMARY  */
    PREDICT = 603,                 /* PREDICT  */
    PRIOR = 604,                   /* PRIOR  */
    PRIVILEGES = 605,              /* PRIVILEGES  */
    PROCEDURAL = 606,              /* PROCEDURAL  */
    PROCEDURE = 607,               /* PROCEDURE  */
    PROCEDURES = 608,              /* PROCEDURES  */
    PROGRAM = 609,                 /* PROGRAM  */
    PUBLICATION = 610,             /* PUBLICATION  */
    QUOTE = 611,                   /* QUOTE  */
    QUOTES = 612,                  /* QUOTES  */
    RANGE = 613,                   /* RANGE  */
    READ = 614,                    /* READ  */
    REAL = 615,                    /* REAL  */
    REASSIGN = 616,                /* REASSIGN  */
    RECURSIVE = 617,               /* RECURSIVE  */
    REF_P = 618,                   /* REF_P  */
    REFERENCES = 619,              /* REFERENCES  */
    REFERENCING = 620,             /* REFERENCING  */
    REFRESH = 621,                 /* REFRESH  */
    REINDEX = 622,                 /* REINDEX  */
    RELATIVE_P = 623,              /* RELATIVE_P  */
    RELEASE = 624,                 /* RELEASE  */
    RENAME = 625,                  /* RENAME  */
    REPEATABLE = 626,              /* REPEATABLE  */
    REPLACE = 627,                 /* REPLACE  */
    REPLICA = 628,                 /* REPLICA  */
    RESET = 629,                   /* RESET  */
    RESTART = 630,                 /* RESTART  */
    RESTRICT = 631,                /* RESTRICT  */
    RETURN = 632,                  /* RETURN  */
    RETURNING = 633,               /* RETURNING  */
    RETURNS = 634,                 /* RETURNS  */
    REVOKE = 635,                  /* REVOKE  */
    RIGHT = 636,                   /* RIGHT  */
    ROLE = 637,                    /* ROLE  */
    ROLLBACK = 638,                /* ROLLBACK  */
    ROLLUP = 639,                  /* ROLLUP  */
    ROUTINE = 640,                 /* ROUTINE  */
    ROUTINES = 641,                /* ROUTINES  */
    ROW = 642,                     /* ROW  */
    ROWS = 643,                    /* ROWS  */
    RULE = 644,                    /* RULE  */
    SAVEPOINT = 645,               /* SAVEPOINT  */
    SCALAR = 646,                  /* SCALAR  */
    SCHEMA = 647,                  /* SCHEMA  */
    SCHEMAS = 648,                 /* SCHEMAS  */
    SCROLL = 649,                  /* SCROLL  */
    SEARCH = 650,                  /* SEARCH  */
    SECOND_P = 651,                /* SECOND_P  */
    SECURITY = 652,                /* SECURITY  */
    SELECT = 653,                  /* SELECT  */
    SEQUENCE = 654,                /* SEQUENCE  */
    SEQUENCES = 655,               /* SEQUENCES  */
    SERIALIZABLE = 656,            /* SERIALIZABLE  */
    SERVER = 657,                  /* SERVER  */
    SESSION = 658,                 /* SESSION  */
    SESSION_USER = 659,            /* SESSION_USER  */
    SET = 660,                     /* SET  */
    SETS = 661,                    /* SETS  */
    SETOF = 662,                   /* SETOF  */
    SHARE = 663,                   /* SHARE  */
    SHOW = 664,                    /* SHOW  */
    SIMILAR = 665,                 /* SIMILAR  */
    SIMPLE = 666,                  /* SIMPLE  */
    SKIP = 667,                    /* SKIP  */
    SMALLINT = 668,                /* SMALLINT  */
    SNAPSHOT = 669,                /* SNAPSHOT  */
    SOME = 670,                    /* SOME  */
    SOURCE = 671,                  /* SOURCE  */
    SQL_P = 672,                   /* SQL_P  */
    STABLE = 673,                  /* STABLE  */
    STANDALONE_P = 674,            /* STANDALONE_P  */
    START = 675,                   /* START  */
    STATEMENT = 676,               /* STATEMENT  */
    STATISTICS = 677,              /* STATISTICS  */
    STDIN = 678,                   /* STDIN  */
    STDOUT = 679,                  /* STDOUT  */
    STORAGE = 680,                 /* STORAGE  */
    STORED = 681,                  /* STORED  */
    STRICT_P = 682,                /* STRICT_P  */
    STRING_P = 683,                /* STRING_P  */
    STRIP_P = 684,                 /* STRIP_P  */
    SUBSCRIPTION = 685,            /* SUBSCRIPTION  */
    SUBSTRING = 686,               /* SUBSTRING  */
    SUPPORT = 687,                 /* SUPPORT  */
    SYMMETRIC = 688,               /* SYMMETRIC  */
    SYSID = 689,                   /* SYSID  */
    SYSTEM_P = 690,                /* SYSTEM_P  */
    SYSTEM_USER = 691,             /* SYSTEM_USER  */
    TABLE = 692,                   /* TABLE  */
    TABLES = 693,                  /* TABLES  */
    TABLESAMPLE = 694,             /* TABLESAMPLE  */
    TABLESPACE = 695,              /* TABLESPACE  */
    TARGET = 696,                  /* TARGET  */
    TEMP = 697,                    /* TEMP  */
    TEMPLATE = 698,                /* TEMPLATE  */
    TEMPORARY = 699,               /* TEMPORARY  */
    TEXT_P = 700,                  /* TEXT_P  */
    THEN = 701,                    /* THEN  */
    TIES = 702,                    /* TIES  */
    TIME = 703,                    /* TIME  */
    TIMESTAMP = 704,               /* TIMESTAMP  */
    TO = 705,                      /* TO  */
    TRAILING = 706,                /* TRAILING  */
    TRANSACTION = 707,             /* TRANSACTION  */
    TRANSFORM = 708,               /* TRANSFORM  */
    TREAT = 709,                   /* TREAT  */
    TRIGGER = 710,                 /* TRIGGER  */
    TRIM = 711,                    /* TRIM  */
    TRUE_P = 712,                  /* TRUE_P  */
    TRUNCATE = 713,                /* TRUNCATE  */
    TRUSTED = 714,                 /* TRUSTED  */
    TYPE_P = 715,                  /* TYPE_P  */
    TYPES_P = 716,                 /* TYPES_P  */
    UESCAPE = 717,                 /* UESCAPE  */
    UNBOUNDED = 718,               /* UNBOUNDED  */
    UNCONDITIONAL = 719,           /* UNCONDITIONAL  */
    UNCOMMITTED = 720,             /* UNCOMMITTED  */
    UNENCRYPTED = 721,             /* UNENCRYPTED  */
    UNION = 722,                   /* UNION  */
    UNIQUE = 723,                  /* UNIQUE  */
    UNKNOWN = 724,                 /* UNKNOWN  */
    UNLISTEN = 725,                /* UNLISTEN  */
    UNLOGGED = 726,                /* UNLOGGED  */
    UNTIL = 727,                   /* UNTIL  */
    UPDATE = 728,                  /* UPDATE  */
    USER = 729,                    /* USER  */
    USING = 730,                   /* USING  */
    VACUUM = 731,                  /* VACUUM  */
    VALID = 732,                   /* VALID  */
    VALIDATE = 733,                /* VALIDATE  */
    VALIDATOR = 734,               /* VALIDATOR  */
    VALUE_P = 735,                 /* VALUE_P  */
    VALUES = 736,                  /* VALUES  */
    VARCHAR = 737,                 /* VARCHAR  */
    VARIADIC = 738,                /* VARIADIC  */
    VARYING = 739,                 /* VARYING  */
    VERBOSE = 740,                 /* VERBOSE  */
    VERSION_P = 741,               /* VERSION_P  */
    VIEW = 742,                    /* VIEW  */
    VIEWS = 743,                   /* VIEWS  */
    VIRTUAL = 744,                 /* VIRTUAL  */
    VOLATILE = 745,                /* VOLATILE  */
    WHEN = 746,                    /* WHEN  */
    WHERE = 747,                   /* WHERE  */
    WHITESPACE_P = 748,            /* WHITESPACE_P  */
    WINDOW = 749,                  /* WINDOW  */
    WITH = 750,                    /* WITH  */
    WITHIN = 751,                  /* WITHIN  */
    WITHOUT = 752,                 /* WITHOUT  */
    WORK = 753,                    /* WORK  */
    WRAPPER = 754,                 /* WRAPPER  */
    WRITE = 755,                   /* WRITE  */
    XML_P = 756,                   /* XML_P  */
    XMLATTRIBUTES = 757,           /* XMLATTRIBUTES  */
    XMLCONCAT = 758,               /* XMLCONCAT  */
    XMLELEMENT = 759,              /* XMLELEMENT  */
    XMLEXISTS = 760,               /* XMLEXISTS  */
    XMLFOREST = 761,               /* XMLFOREST  */
    XMLNAMESPACES = 762,           /* XMLNAMESPACES  */
    XMLPARSE = 763,                /* XMLPARSE  */
    XMLPI = 764,                   /* XMLPI  */
    XMLROOT = 765,                 /* XMLROOT  */
    XMLSERIALIZE = 766,            /* XMLSERIALIZE  */
    XMLTABLE = 767,                /* XMLTABLE  */
    YEAR_P = 768,                  /* YEAR_P  */
    YES_P = 769,                   /* YES_P  */
    ZONE = 770,                    /* ZONE  */
    FORMAT_LA = 771,               /* FORMAT_LA  */
    NOT_LA = 772,                  /* NOT_LA  */
    NULLS_LA = 773,                /* NULLS_LA  */
    WITH_LA = 774,                 /* WITH_LA  */
    WITHOUT_LA = 775,              /* WITHOUT_LA  */
    MODE_TYPE_NAME = 776,          /* MODE_TYPE_NAME  */
    MODE_PLPGSQL_EXPR = 777,       /* MODE_PLPGSQL_EXPR  */
    MODE_PLPGSQL_ASSIGN1 = 778,    /* MODE_PLPGSQL_ASSIGN1  */
    MODE_PLPGSQL_ASSIGN2 = 779,    /* MODE_PLPGSQL_ASSIGN2  */
    MODE_PLPGSQL_ASSIGN3 = 780,    /* MODE_PLPGSQL_ASSIGN3  */
    UMINUS = 781                   /* UMINUS  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 219 "gram.y"

	core_YYSTYPE core_yystype;
	/* these fields must match core_YYSTYPE: */
	int			ival;
	char	   *str;
	const char *keyword;

	char		chr;
	bool		boolean;
	JoinType	jtype;
	DropBehavior dbehavior;
	OnCommitAction oncommit;
	List	   *list;
	Node	   *node;
	ObjectType	objtype;
	TypeName   *typnam;
	FunctionParameter *fun_param;
	FunctionParameterMode fun_param_mode;
	ObjectWithArgs *objwithargs;
	DefElem	   *defelt;
	SortBy	   *sortby;
	WindowDef  *windef;
	JoinExpr   *jexpr;
	IndexElem  *ielem;
	StatsElem  *selem;
	Alias	   *alias;
	RangeVar   *range;
	IntoClause *into;
	WithClause *with;
	InferClause	*infer;
	OnConflictClause *onconflict;
	A_Indices  *aind;
	ResTarget  *target;
	struct PrivTarget *privtarget;
	AccessPriv *accesspriv;
	struct ImportQual *importqual;
	InsertStmt *istmt;
	VariableSetStmt *vsetstmt;
	PartitionElem *partelem;
	PartitionSpec *partspec;
	PartitionBoundSpec *partboundspec;
	RoleSpec   *rolespec;
	PublicationObjSpec *publicationobjectspec;
	struct SelectLimit *selectlimit;
	SetQuantifier setquantifier;
	struct GroupClause *groupclause;
	MergeMatchKind mergematch;
	MergeWhenClause *mergewhen;
	struct KeyActions *keyactions;
	struct KeyAction *keyaction;
	ReturningClause *retclause;
	ReturningOptionKind retoptionkind;

#line 644 "gram.tab.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif

/* Location type.  */
#if ! defined YYLTYPE && ! defined YYLTYPE_IS_DECLARED
typedef struct YYLTYPE YYLTYPE;
struct YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
};
# define YYLTYPE_IS_DECLARED 1
# define YYLTYPE_IS_TRIVIAL 1
#endif




int base_yyparse (core_yyscan_t yyscanner);


#endif /* !YY_BASE_YY_GRAM_TAB_H_INCLUDED  */
