\# 安装deb包

sudo dpkg -i postgresql-predict_18.3-1_amd64.deb



\# 如果缺少依赖，执行

sudo apt-get install -f



\# 初始化数据库

sudo -u postgres /usr/local/pgsql/bin/initdb -D /usr/local/pgsql/data



\# 启动服务

sudo -u postgres /usr/local/pgsql/bin/pg_ctl -D /usr/local/pgsql/data -l /usr/local/pgsql/logfile start



\# 启用pgvector扩展

psql -c "CREATE EXTENSION vector;"