# MindsDB 功能算子迁移分析报告

## 1. 概述

本报告分析 MindsDB 现有的功能算子（ML Handlers），识别哪些算子可以迁移到我们 PostgreSQL 自定义版本的 `predict_function` 和 `embedding_function` 框架中。

### 1.1 我们的框架简介

| 功能 | 接口 | 输入 | 输出 | 触发方式 |
|------|------|------|------|---------|
| `predict_function` | `func(record) → any` | 整行数据（record类型） | 任意类型（写入PREDICT列） | BEFORE INSERT/UPDATE触发器 或 异步后台进程 |
| `embedding_function` | `func(text) → vector` | 文本列值（text类型） | 向量（vector类型） | BEFORE INSERT/UPDATE触发器 + 查询重写 |

### 1.2 迁移评估标准

- **predict_function 迁移条件**：算子的核心功能是对行数据进行推理/预测，输入可映射为 record，输出可映射为标量类型
- **embedding_function 迁移条件**：算子的核心功能是将文本转换为向量嵌入，输入为 text，输出为 vector
- **优先级评估**：基于功能实用性、迁移可行性、用户需求频率

---

## 2. 可迁移到 predict_function 的算子

### 2.1 OpenAI（文本生成/问答）

**算子介绍**：MindsDB 中最全面的 AI 引擎，支持 GPT-3.5/GPT-4 等模型的文本补全、聊天、JSON 结构化提取等功能。支持 5 种运行模式：`default`、`conversational`、`conversational-full`、`image`、`embedding`。

**MindsDB 使用样例**：
```sql
-- MindsDB 中创建模型
CREATE ML_ENGINE openai_engine FROM openai
USING openai_api_key = 'sk-xxx';

CREATE MODEL openai_model
PREDICT answer
USING
    engine = 'openai_engine',
    question_column = 'question';

-- 查询预测
SELECT question, answer
FROM openai_model
WHERE question = 'Where is Stockholm located?';
```

**迁移方案**：将 OpenAI 的文本生成/问答能力封装为 PostgreSQL 的 `predict_function`，通过触发器自动调用。

**迁移后使用样例**：
```sql
-- 创建 OpenAI 预测函数
CREATE OR REPLACE FUNCTION openai_predict(row_data record)
RETURNS text
LANGUAGE plpython3u
AS $$
    import openai, json
    client = openai.OpenAI(api_key='sk-xxx')
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    prompt = data.get('question', json.dumps(data))
    response = client.chat.completions.create(
        model='gpt-4',
        messages=[{'role': 'user', 'content': prompt}]
    )
    return response.choices[0].message.content
$$;

-- 创建带 PREDICT 列的表
CREATE TABLE qa_table (
    id int PRIMARY KEY,
    question text,
    answer text PREDICT
) WITH (
    predict_function = 'openai_predict',
    predict_timing = 'immediate'
);

-- 插入数据时自动生成答案
INSERT INTO qa_table (id, question) VALUES (1, 'Where is Stockholm located?');
-- answer 列自动填充: "Stockholm is located in Sweden."
```

**文档链接**：
- MindsDB OpenAI 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/openai
- 源码：[openai_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/openai_handler/openai_handler.py)

---

### 2.2 Anthropic Claude（文本生成/问答）

**算子介绍**：集成 Anthropic Claude 系列模型（claude-3-opus、claude-3-sonnet 等），支持文本生成和对话。

**MindsDB 使用样例**：
```sql
CREATE ML_ENGINE anthropic_engine FROM anthropic
USING anthropic_api_key = 'sk-ant-xxx';

CREATE MODEL claude_model
PREDICT answer
USING
    engine = 'anthropic_engine',
    model_name = 'claude-3-sonnet-20240229';

SELECT answer FROM claude_model WHERE question = 'Explain quantum computing';
```

**迁移方案**：与 OpenAI 类似，封装为 `predict_function`。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION claude_predict(row_data record)
RETURNS text
LANGUAGE plpython3u
AS $$
    import anthropic
    client = anthropic.Anthropic(api_key='sk-ant-xxx')
    # 从行数据中提取输入
    prompt = str(row_data)  # 简化示例
    message = client.messages.create(
        model='claude-3-sonnet-20240229',
        max_tokens=1024,
        messages=[{'role': 'user', 'content': prompt}]
    )
    return message.content[0].text
$$;

CREATE TABLE ai_responses (
    id int PRIMARY KEY,
    input_text text,
    response text PREDICT
) WITH (
    predict_function = 'claude_predict',
    predict_timing = 'immediate'
);
```

**文档链接**：
- MindsDB Anthropic 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/anthropic
- 源码：[anthropic_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/anthropic_handler/anthropic_handler.py)

---

### 2.3 Ollama（本地 LLM 文本生成）

**算子介绍**：支持在本地部署和运行大语言模型（Llama 3、Mistral 等），无需云服务即可获得 AI 能力。支持 `generate`（文本生成）和 `embedding`（向量嵌入）两种模式。

**MindsDB 使用样例**：
```sql
CREATE ML_ENGINE ollama_engine FROM ollama;

CREATE MODEL llama3_model
PREDICT completion
USING
   engine = 'ollama_engine',
   model_name = 'llama3';

SELECT text, completion
FROM llama3_model
WHERE text = 'Hello';
```

**迁移方案**：封装为 `predict_function`，通过 HTTP 调用本地 Ollama 服务。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION ollama_predict(row_data record)
RETURNS text
LANGUAGE plpython3u
AS $$
    import requests, json
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    prompt = data.get('input_text', json.dumps(data))
    resp = requests.post('http://localhost:11434/api/generate',
        json={'model': 'llama3', 'prompt': prompt, 'stream': False})
    return resp.json()['response']
$$;

CREATE TABLE local_ai (
    id int PRIMARY KEY,
    input_text text,
    completion text PREDICT
) WITH (
    predict_function = 'ollama_predict',
    predict_timing = 'immediate'
);
```

**文档链接**：
- MindsDB Ollama 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/ollama
- 源码：[ollama_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/ollama_handler/ollama_handler.py)

---

### 2.4 LiteLLM（统一 LLM 访问框架）

**算子介绍**：通过 LiteLLM 框架统一访问 OpenAI、Anthropic、Ollama、Google 等多种 LLM 提供商，简化了不同 LLM 的调用接口。

**MindsDB 使用样例**：
```sql
CREATE ML_ENGINE litellm FROM litellm;

CREATE MODEL chat_model
PREDICT answer
USING
    engine = "litellm",
    model = "ollama/llama2:latest",
    base_url = "http://localhost:11434";

SELECT * FROM chat_model WHERE question = "what is ai?";
```

**迁移方案**：封装为 `predict_function`，利用 LiteLLM 的统一接口支持多种 LLM。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION litellm_predict(row_data record)
RETURNS text
LANGUAGE plpython3u
AS $$
    from litellm import completion
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    prompt = data.get('question', str(data))
    model = data.get('model', 'gpt-3.5-turbo')
    response = completion(model=model, messages=[{'role': 'user', 'content': prompt}])
    return response.choices[0].message.content
$$;

CREATE TABLE multi_llm (
    id int PRIMARY KEY,
    question text,
    model text DEFAULT 'gpt-3.5-turbo',
    answer text PREDICT
) WITH (
    predict_function = 'litellm_predict',
    predict_timing = 'immediate'
);
```

**文档链接**：
- MindsDB LiteLLM 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/litellm
- 源码：[litellm_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/litellm_handler/litellm_handler.py)

---

### 2.5 Lightwood（AutoML 分类/回归/时序预测）

**算子介绍**：MindsDB 默认的 AutoML 引擎，基于 Lightwood 库，支持分类、回归和时间序列预测。自动选择最佳模型（Neural、LightGBM、CatBoost 等集成），支持 Shapley 解释和概率输出。

**MindsDB 使用样例**：
```sql
CREATE MODEL home_rentals_model
FROM example_db
  (SELECT * FROM demo_data.home_rentals)
PREDICT rental_price;

-- 预测
SELECT rental_price
FROM home_rentals_model
WHERE sqft = 823
AND number_of_rooms = 2
AND location = 'good';
```

**迁移方案**：将 Lightwood 训练好的模型封装为 `predict_function`，在触发器中调用已训练模型进行推理。这是最核心的迁移场景，因为 Lightwood 的 predict 接口与我们的 predict_function 语义完全匹配。

**迁移后使用样例**：
```sql
-- 1. 先训练模型（通过外部 Python 脚本或 UDF）
-- 假设模型已保存到 /tmp/home_rentals_model

CREATE OR REPLACE FUNCTION lightwood_predict(row_data record)
RETURNS float
LANGUAGE plpython3u
AS $$
    from lightwood import Predictor
    import json
    predictor = Predictor.load('/tmp/home_rentals_model')
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    df = __import__('pandas').DataFrame([data])
    result = predictor.predict(df)
    return float(result['rental_price'].iloc[0])
$$;

-- 2. 创建带 PREDICT 列的表
CREATE TABLE home_rentals (
    id int PRIMARY KEY,
    sqft int,
    number_of_rooms int,
    number_of_bathrooms int,
    location text,
    neighborhood text,
    days_on_market int,
    rental_price float PREDICT
) WITH (
    predict_function = 'lightwood_predict',
    predict_timing = 'deferred'
);

-- 3. 插入数据，后台自动预测
INSERT INTO home_rentals (id, sqft, number_of_rooms, number_of_bathrooms,
                          location, neighborhood, days_on_market)
VALUES (1, 823, 2, 1, 'good', 'downtown', 10);
-- rental_price 列由后台进程自动填充预测结果
```

**文档链接**：
- MindsDB Lightwood 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/lightwood
- 源码：[lightwood_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/lightwood_handler/lightwood_handler.py)

---

### 2.6 PyCaret（AutoML 分类/回归/时序/聚类/异常检测）

**算子介绍**：基于 PyCaret 开源低代码机器学习库，自动化 ML 工作流程。支持 5 种模型类型：`classification`、`regression`、`time_series`、`clustering`、`anomaly`。

**MindsDB 使用样例**：
```sql
CREATE MODEL my_pycaret_class_model
FROM irisdb
    (SELECT SepalLengthCm, SepalWidthCm, PetalLengthCm, PetalWidthCm, Species FROM Iris)
PREDICT Species
USING
  engine = 'pycaret',
  model_type = 'classification',
  model_name = 'xgboost';

SELECT t.Id, m.prediction_label, m.prediction_score
FROM irisdb.Iris as t
JOIN my_pycaret_class_model AS m;
```

**迁移方案**：将 PyCaret 训练好的模型封装为 `predict_function`，支持多种 ML 任务。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION pycaret_predict(row_data record)
RETURNS text
LANGUAGE plpython3u
AS $$
    import joblib, pandas as pd
    model = joblib.load('/tmp/pycaret_classification_model')
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    df = pd.DataFrame([data])
    result = model.predict(df)
    return str(result[0])
$$;

CREATE TABLE iris_predictions (
    id int PRIMARY KEY,
    sepal_length float,
    sepal_width float,
    petal_length float,
    petal_width float,
    species text PREDICT
) WITH (
    predict_function = 'pycaret_predict',
    predict_timing = 'deferred'
);
```

**文档链接**：
- MindsDB PyCaret 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/pycaret
- 源码：[pycaret_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/pycaret_handler/pycaret_handler.py)

---

### 2.7 Anomaly Detection（异常检测）

**算子介绍**：基于 PyOD 和 CatBoost 的异常检测引擎，支持监督、半监督和无监督学习。无监督检测自动生成 `outlier` 列，半监督/监督检测需要标签列。

**MindsDB 使用样例**：
```sql
CREATE ML_ENGINE anomaly_detection_engine FROM anomaly_detection;

-- 无监督检测
CREATE ANOMALY DETECTION MODEL unsupervised_ad
FROM files (SELECT * FROM anomaly_detection)
USING engine = 'anomaly_detection_engine';

SELECT t.class, m.outlier as anomaly
FROM files.anomaly_detection as t
JOIN unsupervised_ad as m;

-- 监督检测
CREATE MODEL supervised_ad
FROM files (SELECT * FROM anomaly_detection)
PREDICT class
USING engine = 'anomaly_detection_engine', type = 'supervised';
```

**迁移方案**：将异常检测模型封装为 `predict_function`，输出为布尔值或异常分数。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION anomaly_predict(row_data record)
RETURNS boolean
LANGUAGE plpython3u
AS $$
    import joblib, pandas as pd
    model = joblib.load('/tmp/anomaly_model')
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    df = pd.DataFrame([data])
    result = model.predict(df)
    return bool(result[0] == 1)
$$;

CREATE TABLE sensor_data (
    id int PRIMARY KEY,
    temperature float,
    pressure float,
    vibration float,
    is_anomaly boolean PREDICT
) WITH (
    predict_function = 'anomaly_predict',
    predict_timing = 'immediate'
);

INSERT INTO sensor_data (id, temperature, pressure, vibration)
VALUES (1, 98.6, 101.3, 0.05);
-- is_anomaly 列自动填充
```

**文档链接**：
- MindsDB Anomaly Detection 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/anomaly-detection
- 源码：[anomaly_detection_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/anomaly_detection_handler/anomaly_detection_handler.py)

---

### 2.8 Cohere（文本生成/摘要）

**算子介绍**：集成 Cohere 企业级 AI 平台，支持文本生成和文本摘要任务。

**MindsDB 使用样例**：
```sql
CREATE ML_ENGINE cohere_engine FROM cohere
USING cohere_api_key = 'your-key';

CREATE MODEL cohere_model
PREDICT answer
USING
    engine = 'cohere_engine',
    task = 'text-generation',
    column = 'question';

SELECT answer FROM cohere_model
WHERE question = 'What is the capital of France?';
```

**迁移方案**：封装为 `predict_function`，支持文本生成和摘要两种任务。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION cohere_predict(row_data record)
RETURNS text
LANGUAGE plpython3u
AS $$
    import cohere
    client = cohere.Client('your-api-key')
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    prompt = data.get('question', str(data))
    response = client.generate(prompt=prompt)
    return response.generations[0].text
$$;

CREATE TABLE cohere_responses (
    id int PRIMARY KEY,
    question text,
    answer text PREDICT
) WITH (
    predict_function = 'cohere_predict',
    predict_timing = 'immediate'
);
```

**文档链接**：
- MindsDB Cohere 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/cohere
- 源码：[cohere_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/cohere_handler/coohere_handler.py)

---

### 2.9 TimeGPT（时间序列预测）

**算子介绍**：集成 Nixtla 的 TimeGPT，专为时间序列预测设计的生成式预训练模型。支持零样本时序预测和异常检测。

**MindsDB 使用样例**：
```sql
CREATE ML_ENGINE timegpt FROM timegpt
USING timegpt_api_key = 'your-key';

CREATE MODEL cryptocurrency_forecast_model
FROM my_binance
  (SELECT * FROM aggregated_trade_data WHERE symbol = 'BTCUSDT')
PREDICT open_price
ORDER BY open_time
HORIZON 10
USING ENGINE = 'timegpt';

SELECT m.open_time, m.open_price
FROM btcusdt_recent AS d
JOIN cryptocurrency_forecast_model AS m
WHERE d.open_time > LATEST;
```

**迁移方案**：将 TimeGPT 的预测能力封装为 `predict_function`，结合 `predict_timing = 'deferred'` 实现批量时序预测。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION timegpt_predict(row_data record)
RETURNS float
LANGUAGE plpython3u
AS $$
    from nixtlats import TimeGPT
    client = TimeGPT(api_key='your-key')
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    # 时间序列预测逻辑
    import pandas as pd
    df = pd.DataFrame([data])
    forecast = client.forecast(df, h=10, time_col='time', target_col='value')
    return float(forecast['TimeGPT'].iloc[0])
$$;

CREATE TABLE time_series_data (
    id int PRIMARY KEY,
    time timestamp,
    value float,
    predicted_value float PREDICT
) WITH (
    predict_function = 'timegpt_predict',
    predict_timing = 'deferred'
);
```

**文档链接**：
- MindsDB TimeGPT 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/timegpt
- 源码：[timegpt_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/timegpt_handler/timegpt_handler.py)

---

### 2.10 BYOM（自定义模型）

**算子介绍**：Bring Your Own Model，允许用户上传自定义的 Python ML 模型代码并在 MindsDB 中使用。支持 `venv`（虚拟环境隔离）和 `inhouse`（进程内执行）两种运行模式。

**MindsDB 使用样例**：
```python
# 自定义模型代码
class CustomPredictor():
    def train(self, df, target_col, args=None):
        <implementation>
        return ''

    def predict(self, df):
        <implementation>
        return df
```

```sql
CREATE MODEL custom_model
FROM my_integration (SELECT * FROM my_table)
PREDICT target
USING ENGINE = 'custom_model_engine';
```

**迁移方案**：BYOM 的理念与我们的 `predict_function` 高度一致——用户自定义函数来处理数据。我们的框架天然支持 BYOM，用户可以直接编写 PostgreSQL 函数（plpython3u/plperl 等）来实现任意预测逻辑。

**迁移后使用样例**：
```sql
-- 用户自定义预测函数（等价于 BYOM 的 predict 方法）
CREATE OR REPLACE FUNCTION my_custom_predict(row_data record)
RETURNS float
LANGUAGE plpython3u
AS $$
    # 任意自定义预测逻辑
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    # 例如：简单的线性回归
    return 0.5 * data.get('feature1', 0) + 1.2 * data.get('feature2', 0) + 3.0
$$;

CREATE TABLE custom_predictions (
    id int PRIMARY KEY,
    feature1 float,
    feature2 float,
    target float PREDICT
) WITH (
    predict_function = 'my_custom_predict',
    predict_timing = 'immediate'
);
```

**文档链接**：
- MindsDB BYOM 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/byom
- 源码：[byom_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/byom_handler/byom_handler.py)

---

### 2.11 HuggingFace（NLP 任务）

**算子介绍**：与 HuggingFace Transformers 模型交互，支持多种 NLP 任务：文本分类、文本生成、零样本分类、翻译、摘要、填空等。

**MindsDB 使用样例**：
```sql
CREATE MODEL sentiment_model
PREDICT sentiment
USING
    engine = 'huggingface',
    task = 'text-classification',
    model_name = 'distilbert-base-uncased-finetuned-sst-2-english';

SELECT sentiment FROM sentiment_model WHERE text = 'I love this product!';
```

**迁移方案**：将 HuggingFace pipeline 封装为 `predict_function`，支持各种 NLP 任务的推理。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION hf_classify_predict(row_data record)
RETURNS text
LANGUAGE plpython3u
AS $$
    from transformers import pipeline
    classifier = pipeline('text-classification',
        model='distilbert-base-uncased-finetuned-sst-2-english')
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    text = data.get('review_text', str(data))
    result = classifier(text)
    return result[0]['label']
$$;

CREATE TABLE reviews (
    id int PRIMARY KEY,
    review_text text,
    sentiment text PREDICT
) WITH (
    predict_function = 'hf_classify_predict',
    predict_timing = 'deferred'
);
```

**文档链接**：
- MindsDB HuggingFace 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/huggingface
- 源码：[huggingface_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/huggingface_handler/huggingface_handler.py)

---

### 2.12 Google Gemini（多模态 AI）

**算子介绍**：集成 Google Generative AI (Gemini) API，支持文本生成、嵌入生成和图像理解三种模式。

**MindsDB 使用样例**：
```sql
CREATE ML_ENGINE gemini_engine FROM google_gemini
USING google_api_key = 'your-key';

CREATE MODEL gemini_model
PREDICT answer
USING engine = 'gemini_engine', model_name = 'gemini-pro';

SELECT answer FROM gemini_model WHERE question = 'Explain AI';
```

**迁移方案**：封装为 `predict_function`，支持 Gemini 的文本生成和视觉理解能力。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION gemini_predict(row_data record)
RETURNS text
LANGUAGE plpython3u
AS $$
    import google.generativeai as genai
    genai.configure(api_key='your-key')
    model = genai.GenerativeModel('gemini-pro')
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    prompt = data.get('question', str(data))
    response = model.generate_content(prompt)
    return response.text
$$;

CREATE TABLE gemini_responses (
    id int PRIMARY KEY,
    question text,
    answer text PREDICT
) WITH (
    predict_function = 'gemini_predict',
    predict_timing = 'immediate'
);
```

**文档链接**：
- MindsDB Google Gemini 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/google-gemini
- 源码：[google_gemini_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/google_gemini_handler/google_gemini_handler.py)

---

### 2.13 LightFM（推荐系统）

**算子介绍**：基于 LightFM 的推荐系统引擎，支持用户-物品推荐和物品-物品推荐，基于协同过滤算法。

**MindsDB 使用样例**：
```sql
CREATE ML_ENGINE lightfm FROM lightfm;

CREATE MODEL lightfm_demo
FROM mysql_demo_db (SELECT * FROM movie_lens_ratings)
PREDICT movieId
USING
  engine = 'lightfm',
  item_id = 'movieId',
  user_id = 'userId',
  threshold = 4,
  n_recommendations = 10;

-- 特定用户的推荐
SELECT b.* FROM lightfm_demo AS b WHERE userId = 100
USING recommender_type = 'user_item';
```

**迁移方案**：将推荐模型封装为 `predict_function`，输入用户信息，输出推荐结果。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION lightfm_predict(row_data record)
RETURNS text
LANGUAGE plpython3u
AS $$
    import joblib, pandas as pd
    model = joblib.load('/tmp/lightfm_model')
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    user_id = data.get('user_id')
    # 获取推荐
    recommendations = model.recommend(user_id, n=10)
    return ','.join(str(r) for r in recommendations)
$$;

CREATE TABLE user_recommendations (
    id int PRIMARY KEY,
    user_id int,
    recommended_items text PREDICT
) WITH (
    predict_function = 'lightfm_predict',
    predict_timing = 'deferred'
);
```

**文档链接**：
- MindsDB LightFM 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/lightfm
- 源码：[lightfm_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/lightfm_handler/lightfm_handler.py)

---

### 2.14 XGBoost（梯度提升）

**算子介绍**：XGBoost 梯度提升框架，支持分类、回归和排序任务。在 MindsDB 中也可通过 PyCaret 引擎的 `model_name = 'xgboost'` 参数使用。

**MindsDB 使用样例**：
```sql
-- 通过 PyCaret 使用 XGBoost
CREATE MODEL xgboost_model
FROM my_db (SELECT * FROM my_table)
PREDICT target
USING
  engine = 'pycaret',
  model_type = 'classification',
  model_name = 'xgboost';
```

**迁移方案**：将 XGBoost 模型封装为 `predict_function`。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION xgboost_predict(row_data record)
RETURNS float
LANGUAGE plpython3u
AS $$
    import xgboost as xgb
    import pandas as pd
    model = xgb.Booster()
    model.load_model('/tmp/xgboost_model.json')
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    df = pd.DataFrame([data])
    dmatrix = xgb.DMatrix(df)
    return float(model.predict(dmatrix)[0])
$$;

CREATE TABLE xgb_predictions (
    id int PRIMARY KEY,
    feature1 float,
    feature2 float,
    target float PREDICT
) WITH (
    predict_function = 'xgboost_predict',
    predict_timing = 'deferred'
);
```

**文档链接**：
- MindsDB XGBoost 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/xgboost
- 源码：[pycaret_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/pycaret_handler/pycaret_handler.py)

---

### 2.15 MLflow（模型服务化）

**算子介绍**：与 MLflow 模型注册表和服务交互，允许在 MindsDB 中使用已部署的 MLflow 模型。

**MindsDB 使用样例**：
```sql
CREATE MODEL mlflow_model
PREDICT target
USING
    engine = 'mlflow',
    model_name = 'my_model',
    mlflow_server_url = 'http://0.0.0.0:5001/',
    predict_url = 'http://localhost:5000/invocations';

SELECT target FROM mlflow_model WHERE text = 'input data';
```

**迁移方案**：将 MLflow 的 HTTP 推理接口封装为 `predict_function`。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION mlflow_predict(row_data record)
RETURNS float
LANGUAGE plpython3u
AS $$
    import requests, json
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    payload = json.dumps({'dataframe_split': {'columns': list(data.keys()),
                                               'data': [list(data.values())]}})
    resp = requests.post('http://localhost:5000/invocations',
                         headers={'Content-Type': 'application/json'},
                         data=payload)
    return float(resp.json()['predictions'][0])
$$;

CREATE TABLE mlflow_predictions (
    id int PRIMARY KEY,
    feature1 float,
    feature2 float,
    target float PREDICT
) WITH (
    predict_function = 'mlflow_predict',
    predict_timing = 'deferred'
);
```

**文档链接**：
- MindsDB MLflow 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/mlflow
- 源码：[mlflow_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/mlflow_handler/mlflow_handler.py)

---

## 3. 可迁移到 embedding_function 的算子

### 3.1 OpenAI Embedding（文本嵌入）

**算子介绍**：OpenAI 引擎的 `embedding` 模式，使用 text-embedding-ada-002 等模型生成文本向量嵌入。

**MindsDB 使用样例**：
```sql
CREATE MODEL openai_embedding_model
PREDICT embedding
USING
    engine = 'openai_engine',
    mode = 'embedding',
    model_name = 'text-embedding-ada-002';

SELECT embedding FROM openai_embedding_model WHERE text = 'Hello world';
```

**迁移方案**：封装为 `embedding_function`，这是最核心的嵌入迁移场景。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION openai_embedding(input_text text)
RETURNS vector
LANGUAGE plpython3u
IMMUTABLE
AS $$
    import openai
    client = openai.OpenAI(api_key='sk-xxx')
    response = client.embeddings.create(
        model='text-embedding-ada-002',
        input=input_text
    )
    embedding = response.data[0].embedding
    return '[' + ','.join(str(x) for x in embedding) + ']'
$$;

CREATE TABLE documents (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    vector_len = 1536,
    embedding_function = 'openai_embedding'
);

-- 插入数据时自动生成嵌入
INSERT INTO documents (id, content) VALUES (1, 'Hello world');
-- content_embedding 列自动填充向量

-- 向量搜索
SELECT * FROM documents
ORDER BY content <-> 'search query'
LIMIT 10;
```

**文档链接**：
- MindsDB OpenAI 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/openai
- 源码：[openai_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/openai_handler/openai_handler.py)

---

### 3.2 Ollama Embedding（本地文本嵌入）

**算子介绍**：Ollama 引擎的 `embedding` 模式，支持在本地生成文本向量嵌入，无需云服务。

**MindsDB 使用样例**：
```sql
CREATE MODEL ollama_embedding_model
PREDICT embedding
USING
   engine = 'ollama_engine',
   model_name = 'nomic-embed-text',
   mode = 'embeddings';

SELECT embedding FROM ollama_embedding_model WHERE text = 'Hello world';
```

**迁移方案**：封装为 `embedding_function`，通过 HTTP 调用本地 Ollama 服务的嵌入 API。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION ollama_embedding(input_text text)
RETURNS vector
LANGUAGE plpython3u
IMMUTABLE
AS $$
    import requests
    resp = requests.post('http://localhost:11434/api/embeddings',
        json={'model': 'nomic-embed-text', 'prompt': input_text})
    embedding = resp.json()['embedding']
    return '[' + ','.join(str(x) for x in embedding) + ']'
$$;

CREATE TABLE local_documents (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    vector_len = 768,
    embedding_function = 'ollama_embedding'
);
```

**文档链接**：
- MindsDB Ollama 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/ollama
- 源码：[ollama_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/ollama_handler/ollama_handler.py)

---

### 3.3 Sentence Transformers（本地文本嵌入）

**算子介绍**：基于 Sentence Transformers 库的文本嵌入生成，支持所有 Sentence Transformers 模型（如 all-MiniLM-L6-v2、all-mpnet-base-v2 等）。

**MindsDB 使用样例**：
```sql
CREATE MODEL sentence_transformers_model
PREDICT embedding
USING
    engine = 'sentence_transformers',
    model_name = 'all-MiniLM-L6-v2';

SELECT embedding FROM sentence_transformers_model WHERE text = 'Hello world';
```

**迁移方案**：封装为 `embedding_function`，这是最重要的本地嵌入迁移场景。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION st_embedding(input_text text)
RETURNS vector
LANGUAGE plpython3u
IMMUTABLE
AS $$
    from sentence_transformers import SentenceTransformer
    model = SentenceTransformer('all-MiniLM-L6-v2')
    embedding = model.encode(input_text)
    return '[' + ','.join(str(x) for x in embedding) + ']'
$$;

CREATE TABLE st_documents (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    vector_len = 384,
    embedding_function = 'st_embedding'
);

-- 向量搜索
SELECT * FROM st_documents
ORDER BY content <-> 'search query'
LIMIT 10;
```

**文档链接**：
- MindsDB Sentence Transformers 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/sentence-transformers
- 源码：[sentence_transformers_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/sentence_transformers_handler/sentence_transformers_handler.py)

---

### 3.4 Google Gemini Embedding（文本嵌入）

**算子介绍**：Google Gemini 引擎的 `embedding` 模式，使用 embedding-001 等模型生成文本向量嵌入。

**MindsDB 使用样例**：
```sql
CREATE MODEL gemini_embedding_model
PREDICT embedding
USING
    engine = 'gemini_engine',
    mode = 'embedding',
    model_name = 'embedding-001';

SELECT embedding FROM gemini_embedding_model WHERE text = 'Hello world';
```

**迁移方案**：封装为 `embedding_function`。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION gemini_embedding(input_text text)
RETURNS vector
LANGUAGE plpython3u
IMMUTABLE
AS $$
    import google.generativeai as genai
    genai.configure(api_key='your-key')
    result = genai.embed_content(model='models/embedding-001',
                                  content=input_text)
    embedding = result['embedding']
    return '[' + ','.join(str(x) for x in embedding) + ']'
$$;

CREATE TABLE gemini_documents (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    vector_len = 768,
    embedding_function = 'gemini_embedding'
);
```

**文档链接**：
- MindsDB Google Gemini 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/google-gemini
- 源码：[google_gemini_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/google_gemini_handler/google_gemini_handler.py)

---

### 3.5 LiteLLM Embedding（统一嵌入接口）

**算子介绍**：LiteLLM 引擎的嵌入功能，通过统一接口访问多种嵌入模型提供商。

**MindsDB 使用样例**：
```sql
CREATE MODEL litellm_embedding_model
PREDICT embedding
USING
    engine = 'litellm',
    model = 'openai/text-embedding-ada-002';

SELECT embedding FROM litellm_embedding_model WHERE text = 'Hello world';
```

**迁移方案**：封装为 `embedding_function`，利用 LiteLLM 统一接口。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION litellm_embedding(input_text text)
RETURNS vector
LANGUAGE plpython3u
IMMUTABLE
AS $$
    from litellm import embedding
    response = embedding(model='openai/text-embedding-ada-002', input=[input_text])
    emb = response.data[0]['embedding']
    return '[' + ','.join(str(x) for x in emb) + ']'
$$;

CREATE TABLE litellm_documents (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    vector_len = 1536,
    embedding_function = 'litellm_embedding'
);
```

**文档链接**：
- MindsDB LiteLLM 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/litellm
- 源码：[litellm_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/litellm_handler/litellm_handler.py)

---

### 3.6 LangChain Embedding（嵌入生成）

**算子介绍**：基于 LangChain 的嵌入生成，支持多种嵌入提供商。

**MindsDB 使用样例**：
```sql
CREATE MODEL langchain_embedding_model
PREDICT embedding
USING
    engine = 'langchain_embedding',
    embedding_provider = 'openai',
    model_name = 'text-embedding-ada-002';
```

**迁移方案**：封装为 `embedding_function`，利用 LangChain 的嵌入接口。

**迁移后使用样例**：
```sql
CREATE OR REPLACE FUNCTION langchain_embedding(input_text text)
RETURNS vector
LANGUAGE plpython3u
IMMUTABLE
AS $$
    from langchain_openai import OpenAIEmbeddings
    embeddings = OpenAIEmbeddings(model='text-embedding-ada-002',
                                   openai_api_key='sk-xxx')
    result = embeddings.embed_query(input_text)
    return '[' + ','.join(str(x) for x in result) + ']'
$$;

CREATE TABLE lc_documents (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    vector_len = 1536,
    embedding_function = 'langchain_embedding'
);
```

**文档链接**：
- MindsDB LangChain Embedding 引擎文档：https://docs.mindsdb.com/integrations/ai-engines/langchain-embedding
- 源码：[langchain_embedding_handler.py](file:///D:/workspace/mindsdb/mindsdb/integrations/handlers/langchain_embedding_handler/langchain_embedding_handler.py)

---

## 4. 迁移优先级与可行性总结

### 4.1 predict_function 迁移优先级

| 优先级 | 算子 | 迁移难度 | 用户需求 | 说明 |
|--------|------|---------|---------|------|
| **P0** | Lightwood (AutoML) | 中 | 高 | 默认AutoML引擎，核心场景 |
| **P0** | OpenAI (文本生成) | 低 | 高 | 最常用的LLM，API简单 |
| **P0** | BYOM (自定义模型) | 低 | 高 | 天然匹配，用户自定义函数 |
| **P1** | Ollama (本地LLM) | 低 | 高 | 本地部署需求大 |
| **P1** | PyCaret (AutoML) | 中 | 中 | 多任务类型支持 |
| **P1** | Anomaly Detection | 中 | 中 | IoT/监控场景需求 |
| **P1** | HuggingFace (NLP) | 中 | 中 | 丰富的NLP任务 |
| **P2** | LiteLLM (统一LLM) | 低 | 中 | 统一接口，简化多LLM调用 |
| **P2** | Anthropic Claude | 低 | 中 | Claude系列模型 |
| **P2** | Cohere | 低 | 中 | 企业AI需求 |
| **P2** | Google Gemini | 低 | 中 | Google生态 |
| **P2** | XGBoost | 中 | 中 | 经典ML算法 |
| **P2** | TimeGPT (时序) | 中 | 中 | 时序预测场景 |
| **P3** | LightFM (推荐) | 中 | 低 | 推荐系统场景 |
| **P3** | MLflow (模型服务) | 中 | 低 | MLOps场景 |

### 4.2 embedding_function 迁移优先级

| 优先级 | 算子 | 迁移难度 | 用户需求 | 说明 |
|--------|------|---------|---------|------|
| **P0** | OpenAI Embedding | 低 | 高 | 最常用的嵌入服务 |
| **P0** | Sentence Transformers | 中 | 高 | 最重要的本地嵌入方案 |
| **P1** | Ollama Embedding | 低 | 高 | 本地嵌入需求 |
| **P1** | LiteLLM Embedding | 低 | 中 | 统一嵌入接口 |
| **P2** | Google Gemini Embedding | 低 | 中 | Google生态 |
| **P2** | LangChain Embedding | 低 | 中 | LangChain生态 |

---

## 5. 架构对比：MindsDB vs 我们的框架

### 5.1 核心差异

| 维度 | MindsDB | 我们的框架 |
|------|---------|-----------|
| **模型管理** | 独立的模型对象（CREATE MODEL） | 函数即模型（CREATE FUNCTION + reloption） |
| **预测触发** | SQL JOIN 查询驱动 | 触发器自动驱动（INSERT/UPDATE时） |
| **训练方式** | 内置训练（CREATE MODEL ... USING） | 外部训练，函数内加载模型 |
| **嵌入存储** | 模型预测结果 | 自动辅助列（_embedding） |
| **向量搜索** | 需要JOIN查询 | 原生SQL操作符（<->, <=>, <#>） |
| **异步预测** | 无内置 | 内置（predict_timing='deferred'） |
| **查询重写** | 无 | 自动重写EMBEDDING列为向量搜索 |

### 5.2 MindsDB 的优势特性（值得借鉴）

1. **模型版本管理**：MindsDB 支持模型版本（finetune 创建新版本）、活跃版本切换
2. **模型描述**：DESCRIBE MODEL 可查看模型信息、特征重要性、训练进度
3. **模型评估**：EVALUATE 命令可在测试数据上评估模型
4. **微调**：FINETUNE 命令支持在已有模型基础上微调
5. **多引擎注册**：CREATE ML ENGINE 统一管理不同 AI 后端
6. **Agent/Skill 系统**：AI Agent + 技能系统，支持 RAG 和 Text2SQL

### 5.3 我们框架的优势特性

1. **零SQL改动**：INSERT 时自动预测，无需 JOIN

2. **原生向量搜索**：EMBEDDING 列自动生成向量，ORDER BY <-> 原生搜索

3. **异步预测**：deferred 模式不阻塞写入

4. **列级函数映射**：支持为不同列指定不同函数

5. **查询重写**：自动将文本搜索转换为向量搜索

6. **PostgreSQL 生态**：完整的事务、并发、索引支持

### 5.4关键发现
   1. BYOM 理念与我们的框架天然匹配 ：MindsDB 的 BYOM 允许用户自定义 Python 模型，我们的 predict_function 本质上就是"用户自定义预测函数"
   2. 我们的框架在自动化方面更优 ：MindsDB 需要 JOIN 查询来触发预测，我们通过触发器自动完成
   3. 我们的向量搜索更原生 ：EMBEDDING 列自动生成向量 + 查询重写，而 MindsDB 需要额外的模型调用来获取嵌入
   4. MindsDB 在模型管理方面更完善 ：支持版本管理、微调、评估、描述等，这些是我们后续可以借鉴的方向

---

## 6. 迁移实施建议

### 6.1 第一阶段：核心算子迁移（P0）

1. **OpenAI predict_function**：封装 OpenAI Chat API，支持文本生成和问答
2. **OpenAI embedding_function**：封装 OpenAI Embeddings API
3. **Sentence Transformers embedding_function**：本地嵌入生成
4. **BYOM predict_function**：完善自定义函数支持文档和示例

### 6.2 第二阶段：扩展算子迁移（P1）

1. **Ollama predict/embedding_function**：本地 LLM 支持
2. **Lightwood/PyCaret predict_function**：AutoML 推理
3. **Anomaly Detection predict_function**：异常检测
4. **HuggingFace predict_function**：NLP 任务推理

### 6.3 第三阶段：高级功能（P2-P3）

1. **LiteLLM predict/embedding_function**：统一 LLM 接口
2. **Anthropic/Cohere/Gemini predict_function**：多 LLM 提供商
3. **TimeGPT predict_function**：时序预测
4. **LightFM predict_function**：推荐系统
5. **MLflow predict_function**：模型服务化

### 6.4 实现模式建议

所有迁移的函数建议遵循统一的实现模式：

```sql
-- predict_function 统一模板
CREATE OR REPLACE FUNCTION <engine>_predict(row_data record)
RETURNS <target_type>
LANGUAGE plpython3u
AS $$
    # 1. 从行数据提取特征
    data = dict(zip(
        [f.name for f in row_data._fields],
        [getattr(row_data, f.name) for f in row_data._fields]
    ))
    # 2. 调用推理引擎
    result = <engine>.predict(data)
    # 3. 返回结果
    return result
$$;

-- embedding_function 统一模板
CREATE OR REPLACE FUNCTION <engine>_embedding(input_text text)
RETURNS vector
LANGUAGE plpython3u
IMMUTABLE
AS $$
    # 1. 调用嵌入模型
    embedding = <engine>.embed(input_text)
    # 2. 返回向量
    return '[' + ','.join(str(x) for x in embedding) + ']'
$$;
```

---

## 7. 参考链接

| 资源 | 链接 |
|------|------|
| MindsDB 官方文档 | https://docs.mindsdb.com/ |
| MindsDB AI 引擎总览 | https://docs.mindsdb.com/integrations/ai-overview |
| MindsDB OpenAI 引擎 | https://docs.mindsdb.com/integrations/ai-engines/openai |
| MindsDB Ollama 引擎 | https://docs.mindsdb.com/integrations/ai-engines/ollama |
| MindsDB LiteLLM 引擎 | https://docs.mindsdb.com/integrations/ai-engines/litellm |
| MindsDB Cohere 引擎 | https://docs.mindsdb.com/integrations/ai-engines/cohere |
| MindsDB PyCaret 引擎 | https://docs.mindsdb.com/integrations/ai-engines/pycaret |
| MindsDB Anomaly Detection 引擎 | https://docs.mindsdb.com/integrations/ai-engines/anomaly-detection |
| MindsDB BYOM 引擎 | https://docs.mindsdb.com/integrations/ai-engines/byom |
| MindsDB LightFM 引擎 | https://docs.mindsdb.com/integrations/ai-engines/lightfm |
| MindsDB TimeGPT 引擎 | https://docs.mindsdb.com/integrations/ai-engines/timegpt |
| MindsDB XGBoost 引擎 | https://docs.mindsdb.com/integrations/ai-engines/xgboost |
| MindsDB MLflow 引擎 | https://docs.mindsdb.com/integrations/ai-engines/mlflow |
| MindsDB Ludwig 引擎 | https://docs.mindsdb.com/integrations/ai-engines/ludwig |
| MindsDB Vertex AI 引擎 | https://docs.mindsdb.com/integrations/ai-engines/vertex |
| MindsDB Sentence Transformers 引擎 | https://docs.mindsdb.com/integrations/ai-engines/sentence-transformers |
| MindsDB HuggingFace 引擎 | https://docs.mindsdb.com/integrations/ai-engines/huggingface |
| MindsDB Google Gemini 引擎 | https://docs.mindsdb.com/integrations/ai-engines/google-gemini |
| MindsDB Portkey 引擎 | https://docs.mindsdb.com/integrations/ai-engines/portkey |
| MindsDB SQL API - CREATE MODEL | https://docs.mindsdb.com/mindsdb_sql/sql/create/model |
| MindsDB SQL API - SELECT | https://docs.mindsdb.com/mindsdb_sql/sql/api/select |
| MindsDB SQL API - JOIN | https://docs.mindsdb.com/mindsdb_sql/sql/api/join |
| MindsDB SQL API - RETRAIN | https://docs.mindsdb.com/mindsdb_sql/sql/api/retrain |
| MindsDB SQL API - FINETUNE | https://docs.mindsdb.com/mindsdb_sql/sql/api/finetune |
| MindsDB SQL API - DESCRIBE | https://docs.mindsdb.com/mindsdb_sql/sql/api/describe |
| MindsDB SQL API - EVALUATE | https://docs.mindsdb.com/mindsdb_sql/sql/api/evaluate |
| MindsDB 模型类型 | https://docs.mindsdb.com/model-types |
| MindsDB 知识库 | https://docs.mindsdb.com/mindsdb_sql/knowledge-bases |
| MindsDB Agents | https://docs.mindsdb.com/mindsdb_sql/agents/agent |
| MindsDB 源码仓库 | https://github.com/mindsdb/mindsdb |
