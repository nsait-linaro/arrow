# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# The following S3 methods are registered on load if dplyr is present

count.arrow_dplyr_query <- function(x, ..., wt = NULL, sort = FALSE, name = NULL) {
  if (!missing(...)) {
    out <- group_by(x, ..., .add = TRUE)
  } else {
    out <- x
  }
  out <- tally(out, wt = {{ wt }}, sort = sort, name = name)

  # Restore original group vars
  gv <- dplyr::group_vars(x)
  if (length(gv)) {
    out$group_by_vars <- gv
  }

  out
}

count.Dataset <- count.ArrowTabular <- count.arrow_dplyr_query

tally.arrow_dplyr_query <- function(x, wt = NULL, sort = FALSE, name = NULL) {
  check_name <- getFromNamespace("check_name", "dplyr")
  name <- check_name(name, dplyr::group_vars(x))

  if (quo_is_null(enquo(wt))) {
    out <- dplyr::summarize(x, !!name := n())
  } else {
    out <- dplyr::summarize(x, !!name := sum({{ wt }}, na.rm = TRUE))
  }

  if (sort) {
    arrange(out, desc(!!sym(name)))
  } else {
    out
  }
}

tally.Dataset <- tally.ArrowTabular <- tally.arrow_dplyr_query
